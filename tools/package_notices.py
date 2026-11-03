# SPDX-License-Identifier: Apache-2.0
#
# Completes a local staged folder with dependency notices, an SPDX inventory and
# the pinned Qt source archive. Does not publish, sign or certify a release.

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import tarfile
from pathlib import Path

import licensecheck


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def installed_ports(installed: Path, triplet: str) -> list[str]:
    """Read installed target packages, including their build dependency notices."""
    names = set()
    for stanza in (installed / "vcpkg/status").read_text(encoding="utf-8").split("\n\n"):
        fields = dict(line.split(": ", 1) for line in stanza.splitlines() if ": " in line and not line.startswith(" "))
        if fields.get("Architecture") != triplet or fields.get("Status") != "install ok installed":
            continue
        name = fields.get("Package", "")
        if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", name):
            raise ValueError("invalid installed package name")
        names.add(name)
    if not names:
        raise ValueError("no installed target packages")
    return sorted(names)


def copy_qt_source(archive: Path, stage: Path, pin: dict) -> list[Path]:
    """Hash before use; copy the archive and only flat regular LICENSES members."""
    if digest(archive) != pin["sha256"]:
        raise ValueError("Qt source archive SHA-256 mismatch")
    prefix = pin["directory"] + "/LICENSES/"
    contents = {}
    with tarfile.open(archive, "r:xz") as source:
        for member in source:
            if not member.name.startswith(prefix) or member.isdir():
                continue
            name = member.name[len(prefix):]
            if not member.isfile() or not re.fullmatch(r"[A-Za-z0-9.+_-]+\.txt", name) or name in contents:
                raise ValueError("invalid Qt license member")
            if member.size > 1024 * 1024:
                raise ValueError("oversized Qt license member")
            stream = source.extractfile(member)
            if stream is None:
                raise ValueError("unreadable Qt license member")
            contents[name] = stream.read()
    for required in ("LGPL-3.0-only.txt", "GPL-3.0-only.txt"):
        if required not in contents:
            raise ValueError("missing Qt license text: " + required)
    target = stage / "sources" / pin["file"]
    if target.name != pin["file"] or Path(pin["file"]).name != pin["file"]:
        raise ValueError("invalid Qt archive filename")
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(archive, target)
    if digest(target) != pin["sha256"]:
        raise ValueError("copied Qt source archive SHA-256 mismatch")
    paths = [target]
    for name, data in sorted(contents.items()):
        path = stage / "licenses/qtbase" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        paths.append(path)
    return paths


def package(repo: Path, stage: Path, installed: Path, triplet: str, qt_root: Path, archive: Path) -> None:
    if not re.fullmatch(r"[a-zA-Z0-9_-]+", triplet):
        raise ValueError("invalid target triplet")
    if not (stage / "frost.exe").is_file():
        raise ValueError("not a staged Windows FrostIDE folder")
    qt = json.loads((repo / "sdk/sdk.lock.json").read_text(encoding="utf-8"))["qt"]
    if not (qt_root / "sbom" / f"qtbase-{qt['version']}.spdx.json").is_file():
        raise ValueError("Qt SBOM version does not match the source pin")
    pin = qt["sources"]["qtbase"]
    report = licensecheck.run(repo, [installed], [stage], qt_root)
    if report.violations:
        raise ValueError("license inventory failed: " + "; ".join(map(str, report.violations)))
    notices = []
    for name in installed_ports(installed, triplet):
        # These two local build helpers install no copyright file. Preserve their
        # original SPDX metadata, not an invented notice; their code is not staged.
        metadata_only = name in {"vcpkg-gn", "vcpkg-tool-gn"}
        filename = "vcpkg.spdx.json" if metadata_only else "copyright"
        suffix = ".spdx.json" if metadata_only else ".txt"
        notices.append((installed / triplet / "share" / name / filename, name + suffix))
    for path, name in notices:
        if not path.is_file():
            raise ValueError("missing upstream notice: " + name)
    paths = copy_qt_source(archive, stage, pin)
    for source, name in notices:
        target = stage / "licenses/dependencies" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        if digest(source) != digest(target):
            raise ValueError("copied dependency notice differs: " + name)
        paths.append(target)
    for relative, name in (("resources/fonts/inter/LICENSE.txt", "Inter.txt"),
                           ("resources/fonts/jetbrains-mono/OFL.txt", "JetBrains-Mono.txt"),
                           ("resources/icons/lucide/LICENSE", "Lucide.txt")):
        target = stage / "licenses" / name
        shutil.copyfile(repo / relative, target)
        paths.append(target)
    sbom = stage / "sbom.spdx.json"
    licensecheck.write_sbom(repo, report, sbom)
    paths.append(sbom)
    note = stage / "licenses/Qt-source.txt"
    note.write_text(f"Qt {qt['version']} is dynamically linked. Its source archive is included at\n"
                    f"sources/{pin['file']}. Original license texts are in licenses/qtbase;\n"
                    "the source archive also preserves component copyright notices and build instructions.\n"
                    "The LGPL permits modification and replacement of the Qt libraries under its terms.\n"
                    "FrostIDE does not verify or restrict replacement Qt DLLs. Compatible replacement DLLs\n"
                    "can be placed beside frost.exe and in its plugin subdirectories.\n"
                    "The SPDX inventory and dependency notices include build and test dependencies;\n"
                    "they are not a list of binaries included in this folder.\n", encoding="utf-8")
    paths.append(note)
    manifest = {"qtSourceUrl": pin["url"], "files": [
        {"path": p.relative_to(stage).as_posix(), "sha256": digest(p)} for p in paths]}
    (stage / "notices-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"package-notices: {len(paths)} files, {len(report.packages)} inventory packages")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("repo", "stage", "installed", "qt-root", "qt-source"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--triplet", required=True)
    args = parser.parse_args()
    try:
        package(args.repo.resolve(), args.stage.resolve(), args.installed.resolve(), args.triplet,
                args.qt_root.resolve(), args.qt_source.resolve())
    except (OSError, ValueError, KeyError, tarfile.TarError) as error:
        parser.exit(1, f"package-notices: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
