# SPDX-License-Identifier: Apache-2.0
"""Offline public-API-double probe; not Unity runtime acceptance. No implicit SDK discovery."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dotnet", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    args = parser.parse_args()
    if not args.dotnet.is_absolute() or not args.dotnet.is_file() or not args.scratch.is_absolute():
        parser.error("explicit installed SDK and absolute scratch parent required")
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="unity-tests-", dir=args.scratch) as temporary:
        root = Path(temporary)
        config = root / "NuGet.Config"
        config.write_text("<configuration><packageSources><clear/></packageSources></configuration>", encoding="utf-8")
        env = os.environ.copy()
        env.update(DOTNET_ROOT=str(args.dotnet.parent), DOTNET_CLI_HOME=str(root / "home"),
                   DOTNET_SKIP_FIRST_TIME_EXPERIENCE="1", DOTNET_CLI_TELEMETRY_OPTOUT="1",
                   DOTNET_NOLOGO="1", DOTNET_GENERATE_ASPNET_CERTIFICATE="0",
                   NUGET_PACKAGES=str(root / "packages"))
        project = Path(__file__).with_name("UnityTestsProbe.csproj")
        output = root / "output"
        subprocess.run([str(args.dotnet), "build", str(project), "--configfile", str(config),
                        "--artifacts-path", str(root / "artifacts"), "--output", str(output)],
                       env=env, cwd=root, check=True, timeout=120)
        subprocess.run([str(args.dotnet), str(output / "UnityTestsProbe.dll")],
                       env=env, cwd=root, check=True, timeout=30)


if __name__ == "__main__":
    main()
