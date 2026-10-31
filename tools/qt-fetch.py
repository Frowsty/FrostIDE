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
# Everything it installs is named in sdk/sdk.lock.json under qt.<platform>:
# the repository, the package, the archive name prefixes, and the trust anchors:
#   updatesSha256   SHA-256 of the repository's Updates.xml;
#   archiveSha256   SHA-256 of every archive file, keyed by archive name.
# An archive without a pin, or whose SHA-256 differs from its pin, is refused.
# The SHA-1 that download.qt.io publishes next to each archive is checked too,
# but only to tell a corrupt download from a changed one; it is not trusted.
#
# Extraction uses bsdtar (libarchive): the in-box tar.exe on Windows, the
# libarchive-tools package on Linux. Its output was compared file by file with
# py7zr's on both platforms when the switch was made (identical contents,
# symlinks and executable bits), so py7zr is no longer a dependency.
#
# Idempotency: after a successful install a marker (.frost-qt-<package>.json)
# records the package, version and archive hashes. When the marker matches the
# pins the script returns at once, without network access. The marker is
# removed before extracting, so an interrupted install is never mistaken for a
# complete one.
#
# Usage:
#   python -I qt-fetch.py --lock sdk/sdk.lock.json --platform windows
#                         --dest E:\sdk\qt --cache E:\sdk\_downloads\qt
#   python -I qt-fetch.py ... --discover
#     Bumping Qt (PLAN.md section 2.3, D20): downloads the archives the lock
#     names without enforcing the archive pins, installs nothing, and prints the
#     archiveSha256 block to review and paste into the lock file.

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


def bsdtar() -> str:
    """Returns the absolute path of bsdtar, or raises when it is not installed."""
    if os.name == "nt":
        path = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "tar.exe")
        if os.path.exists(path):
            return path
    else:
        # GNU tar cannot read .7z, so only bsdtar itself will do.
        path = shutil.which("bsdtar")
        if path:
            return path
    raise RuntimeError("bsdtar not found (Linux: apt install libarchive-tools, see tools/bootstrap-wsl.sh --system)")


def extract_7z(archive: str, dest: str) -> None:
    """Extracts a .7z archive into `dest` with bsdtar."""
    os.makedirs(dest, exist_ok=True)
    subprocess.run([bsdtar(), "-xf", archive, "-C", dest], check=True)


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


def marker_path(dest: str, package: str) -> str:
    """The install marker for `package` under `dest`."""
    return os.path.join(dest, f".frost-qt-{re.sub(r'[^A-Za-z0-9_.-]', '_', package)}.json")


def marker_matches(path: str, package: str, updates_sha256: str, pins: dict) -> bool:
    """True when the marker at `path` records exactly the pinned package and archives."""
    try:
        with open(path, encoding="utf-8") as f:
            record = json.load(f)
    except (OSError, ValueError):
        return False
    installed = {name: entry.get("sha256") for name, entry in record.get("archives", {}).items()}
    return record.get("package") == package and record.get("updatesSha256") == updates_sha256 and installed == pins


def main() -> int:
    parser = argparse.ArgumentParser(description="Installs the Qt subset pinned in sdk/sdk.lock.json.")
    parser.add_argument("--lock", required=True, help="path of sdk/sdk.lock.json")
    parser.add_argument("--platform", required=True, choices=["windows", "linux"])
    parser.add_argument("--dest", required=True, help="install root (the installer's @TargetDir@)")
    parser.add_argument("--cache", required=True, help="download cache directory")
    parser.add_argument("--discover", action="store_true", help="print archive pins for a Qt bump; install nothing")
    args = parser.parse_args()

    with open(args.lock, encoding="utf-8") as f:
        pin = json.load(f)["qt"][args.platform]
    repo = pin["repository"].rstrip("/")
    package_name = pin["package"]
    wanted = pin["archives"]
    updates_pin = pin["updatesSha256"]
    archive_pins = pin.get("archiveSha256", {})
    marker = marker_path(args.dest, package_name)

    if not args.discover and marker_matches(marker, package_name, updates_pin, archive_pins):
        print(f"[qt] {package_name} already installed ({len(archive_pins)} pinned archives)")
        return 0

    updates = http_get(repo + "/Updates.xml")
    updates_hash = hashlib.sha256(updates).hexdigest()
    if updates_hash != updates_pin:
        print(f"error: Updates.xml SHA-256 {updates_hash} != pinned {updates_pin}", file=sys.stderr)
        return 1

    package = find_package(updates, package_name)
    version = package.findtext("Version")
    offered = [a.strip() for a in (package.findtext("DownloadableArchives") or "").split(",") if a.strip()]
    targets = extract_targets(package)

    # An archive is selected when its name starts with "<prefix>-" (qtbase-Windows-...).
    selected = [a for a in offered if any(a.startswith(p + "-") for p in wanted)]
    missing = [p for p in wanted if not any(a.startswith(p + "-") for a in offered)]
    if missing:
        print(f"error: archives not offered by {package_name}: {', '.join(missing)}", file=sys.stderr)
        return 1
    if not args.discover:
        unpinned = [a for a in selected if a not in archive_pins]
        stale = [a for a in archive_pins if a not in selected]
        if unpinned or stale:
            for a in unpinned:
                print(f"error: no archiveSha256 pin for {a}", file=sys.stderr)
            for a in stale:
                print(f"error: pinned archive {a} is not part of the selection", file=sys.stderr)
            print("run with --discover to print the pins for review", file=sys.stderr)
            return 1

    # Download and verify everything before touching the install.
    record = {"package": package_name, "version": version, "updatesSha256": updates_hash, "archives": {}}
    for archive in selected:
        url = f"{repo}/{package_name}/{version}{archive}"
        local = os.path.join(args.cache, f"{version}{archive}")
        sha1_published = http_get(url + ".sha1").decode().split()[0].lower()
        if not os.path.exists(local) or file_digest(local, "sha1") != sha1_published:
            print(f"[qt] downloading {archive}")
            download_to(url, local)
        sha1 = file_digest(local, "sha1")
        if sha1 != sha1_published:
            print(f"error: {archive} SHA-1 {sha1} != published {sha1_published} (corrupt download)", file=sys.stderr)
            return 1
        sha256 = file_digest(local, "sha256")
        if not args.discover and sha256 != archive_pins[archive]:
            print(f"error: {archive} SHA-256 {sha256} != pinned {archive_pins[archive]}", file=sys.stderr)
            return 1
        record["archives"][archive] = {"sha1": sha1, "sha256": sha256}

    if args.discover:
        pins = {name: entry["sha256"] for name, entry in record["archives"].items()}
        print(json.dumps({"updatesSha256": updates_hash, "archiveSha256": pins}, indent=2))
        return 0

    if os.path.exists(marker):
        os.remove(marker)
    for archive in selected:
        target = os.path.join(args.dest, targets.get(archive, ""))
        print(f"[qt] extracting {archive} -> {target}")
        extract_7z(os.path.join(args.cache, f"{version}{archive}"), target)
    with open(marker, "w", encoding="utf-8") as f:
        json.dump(record, f, indent=2)
    print(f"[qt] installed {package_name} {version} ({len(selected)} archives)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
