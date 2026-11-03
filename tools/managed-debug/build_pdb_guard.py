#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Build the source-verification helper in a new private scratch directory; never install globally.
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from audit_inputs import digest
from offline_restore import require_marker

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="new private output directory")
    args = parser.parse_args()
    if not args.sdk_root.is_absolute() or not args.output.is_absolute():
        raise ValueError("SDK and output paths must be absolute")
    sdk = args.sdk_root.resolve(strict=True)
    dotnet = sdk / ("dotnet.exe" if os.name == "nt" else "dotnet")
    if not dotnet.is_file():
        raise ValueError("SDK dotnet executable is missing")
    require_marker()
    output = args.output.resolve()
    output.mkdir(parents=False, exist_ok=False)
    dirs = {name: output / name for name in ("profile", "packages", "http", "plugins", "tmp", "obj", "bin", "build", "helper")}
    for directory in dirs.values():
        directory.mkdir()
    config = output / "NuGet.Config"
    config.write_text("<configuration><packageSources><clear /></packageSources></configuration>", encoding="utf-8")
    profile = dirs["profile"]
    env = os.environ.copy()
    env.update({"HOME": str(profile), "USERPROFILE": str(profile), "APPDATA": str(profile / "appdata"),
                "LOCALAPPDATA": str(profile / "local"), "XDG_CONFIG_HOME": str(profile / "config"),
                "XDG_DATA_HOME": str(profile / "data"), "XDG_CACHE_HOME": str(profile / "cache"),
                "DOTNET_CLI_HOME": str(profile), "DOTNET_ROOT": str(sdk),
                "DOTNET_GENERATE_ASPNET_CERTIFICATE": "false", "DOTNET_CLI_TELEMETRY_OPTOUT": "1",
                "DOTNET_ADD_GLOBAL_TOOLS_TO_PATH": "false", "NUGET_PACKAGES": str(dirs["packages"]),
                "NUGET_HTTP_CACHE_PATH": str(dirs["http"]), "NUGET_PLUGINS_CACHE_PATH": str(dirs["plugins"]),
                "TMP": str(dirs["tmp"]), "TEMP": str(dirs["tmp"]), "TMPDIR": str(dirs["tmp"])})
    env.pop("NUGET_PLUGIN_PATHS", None)
    manifest = json.loads((HERE / "inputs.json").read_text(encoding="utf-8-sig"))
    version = subprocess.run([str(dotnet), "--version"], cwd=output, env=env, check=True,
                             capture_output=True, text=True).stdout.strip()
    if version != manifest["dotnetSdkVersion"]:
        raise ValueError(f"expected SDK {manifest['dotnetSdkVersion']}, got {version}")
    source = REPO / "integrations/managed-debug/PdbGuard"
    project, program = source / "PdbGuard.csproj", source / "Program.cs"
    input_hashes = {p.name: digest(p) for p in (project, program)}
    subprocess.run([str(dotnet), "build", str(project), "--configuration", "Release", "--nologo",
                    "--configfile", str(config), "--output", str(dirs["build"]),
                    f"-p:BaseIntermediateOutputPath={dirs['obj']}{os.sep}",
                    f"-p:BaseOutputPath={dirs['bin']}{os.sep}"], cwd=output, env=env, check=True)
    if any(digest(source / name) != expected for name, expected in input_hashes.items()):
        raise ValueError("helper source changed during build")
    names = ("PdbGuard.dll", "PdbGuard.deps.json", "PdbGuard.runtimeconfig.json")
    for name in names:
        shutil.copyfile(dirs["build"] / name, dirs["helper"] / name)
    result = {"purpose": "scratch-only", "sdk": version, "source": input_hashes,
              "files": {name: digest(dirs["helper"] / name) for name in names}}
    (output / "scratch-manifest.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Built scratch PDB guard {dirs['helper'] / 'PdbGuard.dll'}; not installed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"PDB guard scratch build: {error}", file=sys.stderr)
        raise SystemExit(1)
