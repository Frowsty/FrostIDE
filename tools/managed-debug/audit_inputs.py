#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Audit the exact candidate CoreCLR debugger source and NuGet build inputs. This does not build or install.
from __future__ import annotations

import argparse
import hashlib
import json
import sys
import tarfile
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO / "tools"))
from licensecheck import ALLOWLIST  # noqa: E402


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, required=True, help="directory holding named pinned archives")
    args = parser.parse_args()
    manifest = json.loads((HERE / "inputs.json").read_text(encoding="utf-8-sig"))
    source = manifest["netcoredbg"]
    lock = json.loads((HERE / "packages.lock.json").read_text(encoding="utf-8-sig"))
    locked = lock["dependencies"]["net10.0"]
    expected = {
        "Microsoft.CodeAnalysis.Common": "5.9.0",
        "Microsoft.CodeAnalysis.CSharp": "5.9.0",
        "Microsoft.CodeAnalysis.CSharp.Scripting": "5.9.0",
        "Microsoft.CodeAnalysis.Scripting.Common": "5.9.0",
        "Microsoft.CodeAnalysis.Analyzers": "5.9.0-1.26328.17",
    }
    if set(locked) != set(expected) or any(locked[n]["resolved"] != v for n, v in expected.items()):
        raise ValueError("managed package lock does not contain exactly the reviewed graph")
    files = {p["file"] for p in manifest["packages"]}
    expected_files = {f"{n}.{v}.nupkg".lower() for n, v in expected.items()}
    expected_files.update({
        "microsoft.diagnostics.dbgshim.win-x64.10.0.750501.nupkg",
        "microsoft.diagnostics.dbgshim.linux-x64.10.0.750501.nupkg",
    })
    if len(manifest["packages"]) != len(expected_files) or files != expected_files:
        raise ValueError("input manifest does not contain exactly the reviewed packages")
    records = [source, *manifest["packages"]]
    for item in records:
        name = item.get("file", item.get("archive"))
        path = args.cache / name
        if item["license"] not in ALLOWLIST or digest(path) != item["sha256"]:
            raise ValueError(f"unapproved licence or hash mismatch: {name}")
        if not name.endswith(".nupkg"):
            with tarfile.open(path, "r:gz") as archive:
                license_file = archive.extractfile("netcoredbg-" + item["commit"] + "/LICENSE")
                if license_file is None or hashlib.sha256(license_file.read()).hexdigest() != (
                    "6cd03b0de8299b0800f22b35ae842c931ded7684a2d1ba4f1d4188bab9b09a11"
                ):
                    raise ValueError("netcoredbg licence text mismatch")
            print(f"audited {name}: {item['sha256']} ({item['license']})")
            continue
        with zipfile.ZipFile(path) as archive:
            specs = [n for n in archive.namelist() if n.endswith(".nuspec") and "/" not in n]
            if len(specs) != 1:
                raise ValueError(f"unexpected nuspec count: {name}")
            metadata = ET.fromstring(archive.read(specs[0])).find("{*}metadata")
            license_node = metadata.find("{*}license") if metadata is not None else None
            if license_node is None or license_node.get("type") != "expression" or license_node.text != item["license"]:
                raise ValueError(f"licence declaration mismatch: {name}")
            package_id, version = metadata.findtext("{*}id"), metadata.findtext("{*}version")
            if f"{package_id}.{version}.nupkg".lower() != name:
                raise ValueError(f"package identity mismatch: {name}")
            notice = "ThirdPartyNotices.txt" if "analyzers" in name else "ThirdPartyNotices.rtf"
            if "codeanalysis" in name:
                local = REPO / "third_party" / "managed-notices" / (
                    "roslyn-analyzers-third-party.txt" if "analyzers" in name else "roslyn-third-party.rtf"
                )
                if hashlib.sha256(archive.read(notice)).hexdigest() != digest(local):
                    raise ValueError(f"notice mismatch: {name}")
        print(f"audited {name}: {item['sha256']} ({item['license']})")
    print("Inputs only: native source, runtime provenance, recipe and output distribution remain unapproved.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, zipfile.BadZipFile, tarfile.TarError, ET.ParseError, KeyError) as error:
        print(f"managed-debug input audit: {error}", file=sys.stderr)
        raise SystemExit(1)
