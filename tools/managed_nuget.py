# SPDX-License-Identifier: Apache-2.0
"""Explicit offline SDK build of the read-only package planner; never invoked by opening a workspace."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dotnet", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    if not all(path.is_absolute() for path in (args.dotnet, args.output, args.work)):
        parser.error("dotnet, output and work must be absolute paths")
    if not args.dotnet.is_file():
        parser.error("dotnet executable does not exist")
    if args.work.resolve().is_relative_to(repo) or args.output.resolve().is_relative_to(repo):
        parser.error("build output and work must stay outside the source checkout")
    args.output.mkdir(parents=True, exist_ok=True)
    args.work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="nuget-planner-", dir=args.work) as temporary:
        work = Path(temporary)
        config = work / "NuGet.Config"
        config.write_text('<configuration><packageSources><clear /></packageSources></configuration>', encoding="utf-8")
        project = repo / "tools/managed-nuget/NuGetPlanner.csproj"
        intermediate = str(work / "obj") + os.sep
        common = [str(args.dotnet)]
        subprocess.run(common + ["restore", str(project), "--configfile", str(config),
                                 "-p:BaseIntermediateOutputPath=" + intermediate], check=True, cwd=work)
        subprocess.run(common + ["build", str(project), "--no-restore", "--configuration", "Release",
                                 "--output", str(args.output), "-p:BaseIntermediateOutputPath=" + intermediate],
                       check=True, cwd=work)
        if not (args.output / "NuGetPlanner.dll").is_file():
            raise RuntimeError("SDK did not produce the package edit planner")


if __name__ == "__main__":
    main()
