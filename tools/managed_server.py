#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Verify and stage the pinned managed server with its recorded third-party notices.
"""Offline installer for the pinned csharp-ls NuGet package (ADR 0064)."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import tempfile
import zipfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
PREFIX = "tools/net10.0/any/"


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def verify(archive: Path, root: Path = ROOT) -> tuple[dict, dict]:
    pin = json.loads((root / "sdk/sdk.lock.json").read_text(encoding="utf-8"))["managedServer"]
    manifest_path = root / "sdk/managed-dependencies.json"
    if digest(manifest_path) != pin["manifestSha256"]:
        raise ValueError("managed dependency manifest SHA-256 mismatch")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if digest(archive) != pin["sha256"]:
        raise ValueError("managed server archive SHA-256 mismatch")
    if manifest["server"] != f"{pin['name']}/{pin['version']}":
        raise ValueError("managed server dependency manifest does not match pin")
    for name, expected in manifest["notices"].items():
        if (PurePosixPath(name).name != name or "\\" in name or ":" in name
                or digest(root / "third_party/managed-notices" / name) != expected):
            raise ValueError(f"managed server notice hash mismatch: {name}")
    expected = {f"{p['name']}/{p['version']}" for p in manifest["packages"]}
    if len(expected) != len(manifest["packages"]):
        raise ValueError("duplicate managed dependency")
    with zipfile.ZipFile(archive) as package:
        names = set(package.namelist())
        actual_graphs = {name[len(PREFIX):] for name in names if name.startswith(PREFIX) and name.endswith(".deps.json")}
        if actual_graphs != set(manifest["graphs"]):
            raise ValueError("managed dependency graph list differs")
        discovered = set()
        for path, expected_graph in manifest["graphs"].items():
            deps = json.loads(package.read(PREFIX + path))
            for kind in ("package", "project"):
                actual = {name for name, value in deps["libraries"].items() if value["type"] == kind}
                if actual != set(expected_graph[kind + "s"]):
                    raise ValueError(f"managed dependency graph differs: {path}")
                if kind == "package":
                    discovered.update(actual)
        host_names = {name[len(PREFIX):] for name in names if name.startswith(PREFIX + "BuildHost-") and not name.endswith("/")}
        if host_names != set(manifest["hostAssets"]):
            raise ValueError("managed host asset list differs")
        for path, asset in manifest["hostAssets"].items():
            if hashlib.sha256(package.read(PREFIX + path)).hexdigest() != asset["sha256"]:
                raise ValueError(f"managed host asset differs: {path}")
            if asset["sourcePackage"] not in expected:
                raise ValueError(f"managed host source package missing: {path}")
            if "assetPackage" in asset:
                discovered.add(asset["assetPackage"])
        if discovered != expected:
            raise ValueError(f"managed dependency graph differs: {sorted(discovered ^ expected)}")
        if pin["entry"] not in names:
            raise ValueError("managed server entry is missing")
    return pin, manifest


def audit_packages(cache: Path, manifest: dict) -> None:
    """Verify exact NuGet archive/nuspec/notice hashes and the provenance of host assets."""
    for dependency in manifest["packages"]:
        name, version = dependency["name"], dependency["version"]
        archive = cache / f"{name.lower()}.{version.lower()}.nupkg"
        if digest(archive) != dependency["archiveSha256"]:
            raise ValueError(f"managed dependency archive differs: {name}/{version}")
        with zipfile.ZipFile(archive) as package:
            specs = [item for item in package.namelist() if item.lower().endswith(".nuspec")]
            if len(specs) != 1 or hashlib.sha256(package.read(specs[0])).hexdigest() != dependency["nuspecSha256"]:
                raise ValueError(f"managed dependency nuspec differs: {name}/{version}")
            document = ET.fromstring(package.read(specs[0]))
            metadata = next(child for child in document if child.tag.split("}")[-1] == "metadata")
            fields = {child.tag.split("}")[-1]: (child.text or "").strip() for child in metadata}
            if (fields.get("id", "").casefold() != name.casefold() or fields.get("version") != version
                    or fields.get("authors") != dependency["authors"]
                    or fields.get("copyright") != dependency["copyright"]):
                raise ValueError(f"managed dependency attribution differs: {name}/{version}")
            declared = fields.get("license")
            if not declared and (name, version) == ("FSharp.Control.AsyncSeq", "3.2.1"):
                declared = "Apache-2.0"  # Historical upstream licence pinned in ADR 0064.
            if declared != dependency["license"]:
                raise ValueError(f"managed dependency licence differs: {name}/{version}")
            found_notices = set()
            for item in package.namelist():
                base = item.lower().rsplit("/", 1)[-1]
                if (not item.endswith("/") and any(word in base for word in ("license", "licence", "notice", "thirdparty"))
                        and not base.endswith((".dll", ".pdb", ".xml"))):
                    found_notices.add(hashlib.sha256(package.read(item)).hexdigest())
            recorded = {manifest["notices"][entry] for entry in dependency["notices"]}
            if found_notices != recorded:
                raise ValueError(f"managed dependency notices differ: {name}/{version}")
            identity = f"{name}/{version}"
            for path, asset in manifest["hostAssets"].items():
                for identity_key, member_key in (("sourcePackage", "sourceMember"), ("assetPackage", "assetMember")):
                    if asset.get(identity_key) == identity:
                        if hashlib.sha256(package.read(asset[member_key])).hexdigest() != asset["sha256"]:
                            raise ValueError(f"managed host asset provenance differs: {path}")


def install(archive: Path, destination: Path, root: Path = ROOT) -> None:
    _, manifest = verify(archive, root)
    if destination.exists():
        raise ValueError(f"destination already exists: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".managed-stage-", dir=destination.parent))
    try:
        with zipfile.ZipFile(archive) as package:
            for item in package.infolist():
                if not item.filename.startswith(PREFIX) or item.is_dir():
                    continue
                relative = PurePosixPath(item.filename[len(PREFIX):])
                if relative.is_absolute() or not relative.parts or any(part in ("", ".", "..") for part in relative.parts):
                    raise ValueError("unsafe archive path")
                if "\\" in str(relative) or ":" in str(relative) or stat.S_ISLNK(item.external_attr >> 16):
                    raise ValueError("unsafe archive entry")
                target = staging.joinpath(*relative.parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                with package.open(item) as source, target.open("xb") as output:
                    shutil.copyfileobj(source, output)
        notices = staging / "FrostIDE-Notices"
        notices.mkdir()
        shutil.copy2(root / "sdk/managed-dependencies.json", notices / "managed-dependencies.json")
        for name in manifest["notices"]:
            shutil.copy2(root / "third_party/managed-notices" / name, notices / name)
        os.rename(staging, destination)
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def verify_installed(archive: Path, destination: Path, root: Path = ROOT) -> None:
    """Compare each installed server and notice file with the pinned archive and source notices."""
    _, manifest = verify(archive, root)
    expected: dict[str, str] = {}
    with zipfile.ZipFile(archive) as package:
        for item in package.infolist():
            if not item.filename.startswith(PREFIX) or item.is_dir():
                continue
            relative = PurePosixPath(item.filename[len(PREFIX):])
            if relative.is_absolute() or not relative.parts or any(part in ("", ".", "..") for part in relative.parts):
                raise ValueError("unsafe archive path")
            if "\\" in str(relative) or ":" in str(relative) or stat.S_ISLNK(item.external_attr >> 16):
                raise ValueError("unsafe archive entry")
            with package.open(item) as source:
                h = hashlib.sha256()
                for chunk in iter(lambda: source.read(1024 * 1024), b""):
                    h.update(chunk)
            expected[str(relative)] = h.hexdigest()
    for name, value in manifest["notices"].items():
        expected["FrostIDE-Notices/" + name] = value
    expected["FrostIDE-Notices/managed-dependencies.json"] = digest(root / "sdk/managed-dependencies.json")
    actual = {p.relative_to(destination).as_posix() for p in destination.rglob("*") if p.is_file()}
    if actual != expected.keys():
        raise ValueError("installed managed server file list differs")
    for name, value in expected.items():
        if digest(destination / name) != value:
            raise ValueError(f"installed managed server file changed: {name}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path, help="downloaded pinned NuGet archive")
    parser.add_argument("--dest", required=True, type=Path, help="new server installation directory")
    parser.add_argument("--verify-only", action="store_true", help="verify an existing installation")
    parser.add_argument("--audit-cache", type=Path, help="audit every exact dependency NuGet archive in this directory")
    args = parser.parse_args()
    try:
        if args.audit_cache:
            _, manifest = verify(args.archive, ROOT)
            audit_packages(args.audit_cache, manifest)
        if args.verify_only:
            verify_installed(args.archive, args.dest)
        else:
            install(args.archive, args.dest)
    except (OSError, ValueError, KeyError, StopIteration, ET.ParseError, zipfile.BadZipFile) as error:
        parser.exit(1, f"managed server: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
