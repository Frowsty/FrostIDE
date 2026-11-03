# SPDX-License-Identifier: Apache-2.0
# Build the Windows .NET Framework ProjectHost against the installed VS MSBuild.
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    if not all(path.is_absolute() for path in (args.output, args.work, args.sdk)):
        raise ValueError("output, work and SDK paths must be absolute")
    lock = json.loads((repo / "sdk/sdk.lock.json").read_text(encoding="utf-8"))
    dotnet = args.sdk / ("dotnet-" + lock["windows"]["dotnet"]["version"]) / "dotnet.exe"
    program_files = os.environ.get("ProgramFiles(x86)", "")
    if not Path(program_files).is_absolute():
        raise ValueError("ProgramFiles(x86) must be an absolute directory")
    vswhere = Path(program_files) / "Microsoft Visual Studio/Installer/vswhere.exe"
    if not dotnet.is_file() or not vswhere.is_file():
        raise FileNotFoundError("Pinned dotnet SDK or Visual Studio locator not found")
    found = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.Component.MSBuild",
                            "-property", "installationPath"], capture_output=True, text=True, check=True)
    installation = Path(found.stdout.strip())
    if not installation.is_absolute():
        raise ValueError("Visual Studio locator returned a non-absolute installation")
    msbuild = installation / "MSBuild/Current/Bin/amd64"
    if not (msbuild / "Microsoft.Build.dll").is_file():
        raise FileNotFoundError("Visual Studio MSBuild assemblies not found")
    for directory in (args.output, args.work / "profile", args.work / "temp", args.work / "empty-source"):
        directory.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.pop("NUGET_PLUGIN_PATHS", None)
    profile = args.work / "profile"
    env.update({"HOME": str(profile), "USERPROFILE": str(profile), "APPDATA": str(profile / "appdata"),
                "LOCALAPPDATA": str(profile / "local"), "TEMP": str(args.work / "temp"),
                "TMP": str(args.work / "temp"), "DOTNET_CLI_HOME": str(profile),
                "NUGET_PACKAGES": str(args.work / "nuget/packages"),
                "NUGET_HTTP_CACHE_PATH": str(args.work / "nuget/http"),
                "NUGET_PLUGINS_CACHE_PATH": str(args.work / "nuget/plugins"),
                "DOTNET_GENERATE_ASPNET_CERTIFICATE": "false", "DOTNET_CLI_TELEMETRY_OPTOUT": "1",
                "DOTNET_ADD_GLOBAL_TOOLS_TO_PATH": "false"})
    subprocess.run([str(dotnet), "build", str(repo / "tools/project-host-spike/ProjectHostVsProbe.csproj"),
                    "--configuration", "Release", "--output", str(args.output), "--source", str(args.work / "empty-source"),
                    "-p:BaseIntermediateOutputPath=" + str(args.work / "obj") + os.sep,
                    "-p:VisualStudioMSBuildDir=" + str(msbuild)], env=env, check=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print("ProjectHost build failed:", error, file=sys.stderr)
        sys.exit(1)
