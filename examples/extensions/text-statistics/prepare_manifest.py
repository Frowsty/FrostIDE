# SPDX-License-Identifier: Apache-2.0
"""Offline manifest preparation; reads explicitly named local files, never executes a tool."""
import argparse
import hashlib
import json
from pathlib import Path


def prepare(executable: Path, license_file: Path, dependencies: list[Path]) -> dict:
    executable = executable.resolve(strict=True)
    license_file = license_file.resolve(strict=True)
    files = [executable, *(p.resolve(strict=True) for p in dependencies)]
    if len(files) > 32 or len(set(files)) != len(files):
        raise ValueError("At most 32 unique executable/runtime files are supported")
    if len({p.name.lower() for p in files}) != len(files):
        raise ValueError("Flat sample staging requires unique dependency basenames")
    artifacts = []
    total = 0
    for path in files:
        if not path.is_file() or path.stat().st_size > 128 * 1024 * 1024:
            raise ValueError("Expected bounded regular executable/runtime file")
        data = path.read_bytes()
        total += len(data)
        if total > 256 * 1024 * 1024:
            raise ValueError("Sample runtime exceeds 256 MiB")
        artifacts.append({"path": str(path), "sha256": hashlib.sha256(data).hexdigest()})
    license_bytes = license_file.read_bytes()
    if not license_bytes or len(license_bytes) > 1024 * 1024:
        raise ValueError("Expected bounded nonempty Apache-2.0 license evidence")
    server = {"id": "sample-text-statistics", "name": "Frost text-statistics sample", "languageId": "frostnote",
              "version": "1", "enabled": True, "extensions": [".frostnote"], "executable": str(executable),
              "arguments": [], "license": "Apache-2.0", "licensePath": str(license_file),
              "licenseSha256": hashlib.sha256(license_bytes).hexdigest(), "artifacts": artifacts,
              "sandbox": {"profile": "read-only-stdio", "files": [
                  {"path": str(path), "relativePath": path.name, "executable": True} for path in files]}}
    return {"apiVersion": 1, "extensions": [{"id": "text-statistics", "name": "Text statistics sample",
        "version": "1", "enabled": True, "capabilities": ["languages", "commands"], "contributes": {
            "languageServers": [server], "commands": [{"id": "count", "title": "Count received note text",
                "serverId": "sample-text-statistics", "command": "frost.sample.statistics", "arguments": []}]}}]}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--license", type=Path, required=True)
    parser.add_argument("--dependency", type=Path, action="append", default=[])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    manifest = prepare(args.executable, args.license, args.dependency)
    with args.output.open("x", encoding="utf-8") as output:
        json.dump(manifest, output, indent=2)
        output.write("\n")


if __name__ == "__main__":
    main()
