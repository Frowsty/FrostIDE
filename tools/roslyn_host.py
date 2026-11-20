#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Offline owned-host builder and receipt verifier over the already audited managed dependency graph.
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import managed_server
ROOT = Path(__file__).resolve().parent.parent
ENTRY = "FrostRoslynHost.dll"
RECORD = "frost-roslyn-payload.json"
def sha(path: Path) -> str:
    return managed_server.digest(path)
def source_inputs(root: Path = ROOT) -> dict[str, str]:
    source = root / "integrations/roslyn-host"
    return {str(p.relative_to(root)).replace("\\", "/"): sha(p) for p in sorted(source.iterdir()) if p.is_file() and p.suffix in (".cs", ".csproj", ".json")}
def verify_payload(destination: Path, root: Path = ROOT) -> None:
    record = json.loads((destination / RECORD).read_text(encoding="utf-8"))
    pin = json.loads((root / "sdk/sdk.lock.json").read_text(encoding="utf-8"))
    if record.get("schemaVersion") != 1 or record.get("sourceInputs") != source_inputs(root) or record.get("dependencyManifest") != sha(root / "sdk/managed-dependencies.json"):
        raise ValueError("RoslynHost source/dependency receipt differs")
    if record.get("sdkVersion") != pin["windows"]["dotnet"]["version"]:
        raise ValueError("RoslynHost SDK receipt differs")
    files = record.get("files", {})
    if ENTRY not in files or len(files) > 4096:
        raise ValueError("RoslynHost receipt lacks its entry or exceeds file budget")
    actual = {p.relative_to(destination).as_posix() for p in destination.rglob("*") if p.is_file() and p.name != RECORD}
    if actual != files.keys():
        raise ValueError("RoslynHost payload file list differs")
    for name, digest in files.items():
        relative = Path(name)
        if relative.is_absolute() or ".." in relative.parts or "\\" in name or ":" in name or sha(destination / relative) != digest:
            raise ValueError("RoslynHost payload file changed: " + name)
def build_payload(dotnet: Path, assets: Path, archive: Path, destination: Path, root: Path = ROOT) -> None:
    if not dotnet.is_absolute() or not dotnet.is_file() or not assets.is_absolute() or not destination.is_absolute():
        raise ValueError("RoslynHost tools/assets/output must be explicit absolute paths")
    if destination.exists():
        raise ValueError("RoslynHost destination already exists; verify it or select a new directory")
    managed_server.verify_installed(archive, assets, root)
    manifest = json.loads((root / "sdk/managed-dependencies.json").read_text(encoding="utf-8"))
    decompiler = [p for p in manifest["packages"] if p["name"] == "ICSharpCode.Decompiler"]
    if len(decompiler) != 1 or decompiler[0]["version"] != "9.1.0.7988" or decompiler[0]["license"] != "MIT":
        raise ValueError("Library source provider requires the audited ICSharpCode.Decompiler 9.1.0.7988 MIT record")
    if not (assets / "ICSharpCode.Decompiler.dll").is_file():
        raise ValueError("Verified managed assets lack the decompiler engine")
    lock = json.loads((root / "sdk/sdk.lock.json").read_text(encoding="utf-8"))
    expected_sdk = lock["windows" if os.name == "nt" else "linux"]["dotnet"]["version"]
    source = source_inputs(root)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="frost-roslyn-", dir=destination.parent) as temporary:
        scratch = Path(temporary)
        project = scratch / "source"
        shutil.copytree(root / "integrations/roslyn-host", project)
        profile = scratch / "profile"
        environment = os.environ.copy()
        environment.update(DOTNET_CLI_HOME=str(profile), DOTNET_SKIP_FIRST_TIME_EXPERIENCE="1", DOTNET_CLI_TELEMETRY_OPTOUT="1", DOTNET_GENERATE_ASPNET_CERTIFICATE="0", DOTNET_ADD_GLOBAL_TOOLS_TO_PATH="0", DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE="true", NUGET_PACKAGES=str(scratch / "packages"))
        version = subprocess.run([str(dotnet), "--version"], cwd=project, env=environment, check=True, capture_output=True, text=True).stdout.strip()
        if version != expected_sdk:
            raise ValueError("RoslynHost requires pinned SDK " + expected_sdk)
        configuration = scratch / "NuGet.Config"
        configuration.write_text('<configuration><packageSources><clear /></packageSources></configuration>', encoding="utf-8")
        assembly_output = scratch / "payload"
        common = ["-p:FrostManagedAssets=" + str(assets)]
        subprocess.run([str(dotnet), "restore", str(project / "FrostRoslynHost.csproj"), "--configfile", str(configuration), *common], cwd=project, env=environment, check=True)
        subprocess.run([str(dotnet), "build", str(project / "FrostRoslynHost.csproj"), "--no-restore", "--configuration", "Release", "--output", str(assembly_output), *common], cwd=project, env=environment, check=True)
        if not (assembly_output / "ICSharpCode.Decompiler.dll").is_file() or sha(assembly_output / "ICSharpCode.Decompiler.dll") != sha(assets / "ICSharpCode.Decompiler.dll"):
            raise ValueError("Published decompiler differs from the verified managed payload")
        if source_inputs(root) != source:
            raise ValueError("RoslynHost source changed during build")
        files = {p.relative_to(assembly_output).as_posix(): sha(p) for p in sorted(assembly_output.rglob("*")) if p.is_file()}
        receipt = {"schemaVersion": 1, "sourceInputs": source, "dependencyManifest": sha(root / "sdk/managed-dependencies.json"), "sdkVersion": expected_sdk, "files": files}
        (assembly_output / RECORD).write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        verify_payload(assembly_output, root)
        assembly_output.rename(destination)
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dotnet", type=Path)
    parser.add_argument("--assets", type=Path)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--dest", type=Path, required=True)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    try:
        if args.verify_only: verify_payload(args.dest)
        else:
            if not all((args.dotnet, args.assets, args.archive)): raise ValueError("build requires dotnet, assets and archive")
            build_payload(args.dotnet, args.assets, args.archive, args.dest)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, f"RoslynHost: {error}\n")
    return 0
if __name__ == "__main__":
    raise SystemExit(main())
