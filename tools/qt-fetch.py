# SPDX-License-Identifier: Apache-2.0
#
# qt-fetch.py: installs a pinned subset of a prebuilt Qt from the official Qt
# online repository (download.qt.io) without the Qt online installer.
#
# Why this exists: aqtinstall 3.3.0 cannot locate Qt 6.12.0 for Windows, because
# the Windows repository moved to one directory per compiler
# (qt6_6120/qt6_6120_msvc2022_64/Updates.xml). Spike S3 records the details.
# The Linux repository still uses the old layout; this script handles both.
#
# Trust chain: Updates.xml is checked against a SHA-256 pinned in
# sdk/sdk.lock.json (or against the repository's Updates.xml.sha256 when no pin
# exists yet). Every archive is checked against the SHA-1 published next to it
# on download.qt.io over HTTPS, and its SHA-256 is printed so it can be pinned.
#
# Usage:
#   python qt-fetch.py --repository URL --package NAME --archives qtbase,qtsvg
#                      --dest E:\sdk\qt --cache E:\sdk\_downloads\qt
#                      [--updates-sha256 HEX]

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import xml.etree.ElementTree as ET

# Network timeout for every HTTP request, in seconds.
HTTP_TIMEOUT_S = 120


def http_get(url: str) -> bytes:
    """Downloads `url` fully into memory; raises on any HTTP error."""
    request = urllib.request.Request(url, headers={"User-Agent": "frostide-qt-fetch/1"})
    with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT_S) as response:
        return response.read()


def download_to(url: str, path: str) -> None:
    """Streams `url` to `path` through a temporary file so a partial download never looks complete."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    request = urllib.request.Request(url, headers={"User-Agent": "frostide-qt-fetch/1"})
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), suffix=".part")
    try:
        with os.fdopen(fd, "wb") as out, urllib.request.urlopen(request, timeout=HTTP_TIMEOUT_S) as response:
            shutil.copyfileobj(response, out, length=1 << 20)
        os.replace(tmp, path)
    except BaseException:
        os.unlink(tmp)
        raise


def file_digest(path: str, algorithm: str) -> str:
    """Returns the hex digest of a file, read in 1 MiB chunks."""
    h = hashlib.new(algorithm)
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def extract_7z(archive: str, dest: str) -> None:
    """Extracts a .7z archive with py7zr when available, otherwise with bsdtar or 7z."""
    os.makedirs(dest, exist_ok=True)
    try:
        import py7zr  # Optional dependency; installed by bootstrap into the tools venv.

        with py7zr.SevenZipFile(archive, mode="r") as z:
            z.extractall(path=dest)
        return
    except ImportError:
        pass
    for tool in (["bsdtar", "-xf", archive, "-C", dest], ["7z", "x", "-y", "-o" + dest, archive]):
        if shutil.which(tool[0]):
            subprocess.run(tool, check=True)
            return
    raise RuntimeError("no 7z extractor found: install py7zr, bsdtar (libarchive-tools) or 7z")


def find_package(updates_xml: bytes, name: str) -> ET.Element:
    """Returns the <PackageUpdate> element whose <Name> equals `name`."""
    root = ET.fromstring(updates_xml)
    for package in root.iter("PackageUpdate"):
        if package.findtext("Name") == name:
            return package
    raise KeyError(f"package {name} not found in Updates.xml")


def extract_targets(package: ET.Element) -> dict:
    """
    Maps each archive file name to the directory its Extract operation names,
    relative to the installer's @TargetDir@ (e.g. "6.12.0/msvc2022_64/bin").
    """
    targets = {}
    for op in package.iter("Operation"):
        if op.get("name") != "Extract":
            continue
        args = [a.text or "" for a in op.findall("Argument")]
        if len(args) == 2:
            target = args[0].replace("@TargetDir@", "").lstrip("/\\")
            targets[args[1]] = target
    return targets


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True, help="URL of the directory containing Updates.xml")
    parser.add_argument("--package", required=True, help="package name, e.g. qt.qt6.6120.win64_msvc2022_64")
    parser.add_argument("--archives", required=True, help="comma-separated archive name prefixes, e.g. qtbase,qtsvg")
    parser.add_argument("--dest", required=True, help="install root (the installer's @TargetDir@)")
    parser.add_argument("--cache", required=True, help="download cache directory")
    parser.add_argument("--updates-sha256", default=None, help="pinned SHA-256 of Updates.xml")
    args = parser.parse_args()

    repo = args.repository.rstrip("/")
    updates = http_get(repo + "/Updates.xml")
    updates_hash = hashlib.sha256(updates).hexdigest()
    expected = args.updates_sha256
    if not expected:
        # No pin yet: fall back to the checksum the repository publishes and print
        # the value so it can be pinned in sdk/sdk.lock.json.
        expected = http_get(repo + "/Updates.xml.sha256").decode().split()[0]
        print(f"note: Updates.xml is not pinned; repository checksum {expected}")
    if updates_hash != expected:
        print(f"error: Updates.xml SHA-256 {updates_hash} != expected {expected}", file=sys.stderr)
        return 1

    package = find_package(updates, args.package)
    version = package.findtext("Version")
    archives = [a.strip() for a in (package.findtext("DownloadableArchives") or "").split(",") if a.strip()]
    targets = extract_targets(package)
    wanted = [p.strip() for p in args.archives.split(",") if p.strip()]

    # An archive is selected when its name starts with "<prefix>-" (qtbase-Windows-...).
    selected = [a for a in archives if any(a.startswith(p + "-") for p in wanted)]
    missing = [p for p in wanted if not any(a.startswith(p + "-") for a in archives)]
    if missing:
        print(f"error: archives not offered by {args.package}: {', '.join(missing)}", file=sys.stderr)
        return 1

    record = {"package": args.package, "version": version, "updatesSha256": updates_hash, "archives": {}}
    for archive in selected:
        url = f"{repo}/{args.package}/{version}{archive}"
        local = os.path.join(args.cache, f"{version}{archive}")
        sha1_expected = http_get(url + ".sha1").decode().split()[0].lower()
        if not os.path.exists(local) or file_digest(local, "sha1") != sha1_expected:
            print(f"downloading {archive}")
            download_to(url, local)
        sha1_actual = file_digest(local, "sha1")
        if sha1_actual != sha1_expected:
            print(f"error: {archive} SHA-1 {sha1_actual} != published {sha1_expected}", file=sys.stderr)
            return 1
        target = os.path.join(args.dest, targets.get(archive, ""))
        print(f"extracting {archive} -> {target}")
        extract_7z(local, target)
        record["archives"][archive] = {"sha1": sha1_actual, "sha256": file_digest(local, "sha256")}

    # The marker records exactly what was installed; bootstrap compares it to decide whether to skip.
    marker = os.path.join(args.dest, f".frost-qt-{re.sub(r'[^A-Za-z0-9_.-]', '_', args.package)}.json")
    with open(marker, "w", encoding="utf-8") as f:
        json.dump(record, f, indent=2)
    print(f"installed {args.package} {version} ({len(selected)} archives)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
