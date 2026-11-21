# SPDX-License-Identifier: Apache-2.0
# Splits debug sections from staged FrostIDE ELF images and archives their verified pairs.

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile
import zipfile


SCHEMA = "frost-elf-symbol-archive/1"


def digest(path: Path) -> str:
    with path.open("rb") as file:
        return hashlib.file_digest(file, "sha256").hexdigest()


def inspect(readelf: Path, image: Path) -> tuple[str, bool]:
    with image.open("rb") as file:
        if file.read(4) != b"\x7fELF":
            raise ValueError(f"expected a FrostIDE ELF image: {image.name}")
    details = subprocess.check_output([readelf, "-n", "-SW", image], text=True)
    ids = re.findall(r"Build ID: ([0-9a-f]+)\b", details)
    if len(ids) > 1:
        raise ValueError(f"ambiguous GNU build ID: {image.name}")
    debug = bool(re.search(r"\[\s*\d+\]\s+\.(?:z?debug_|gnu\.debuglto_)", details))
    return ids[0] if ids else "", debug


def split_stage(stage: Path, archive: Path, tools: Path, images: list[str], replace: bool = False) -> None:
    if archive.parent.resolve() != stage.parent.resolve() or archive == stage or archive.is_symlink():
        raise ValueError("ELF symbol archive must be beside the stage")
    if archive.exists() and not replace:
        raise ValueError(f"symbol archive already exists: {archive}")
    if len(images) != len(set(images)):
        raise ValueError("duplicate staged ELF image name")
    objcopy, readelf = tools / "llvm-objcopy", tools / "llvm-readelf"
    with tempfile.TemporaryDirectory(prefix=".frost-elf-symbols-", dir=stage.parent) as scratch:
        temporary = Path(scratch)
        modules = []
        changed = []
        for image in images:
            if Path(image).name != image or not image or image in (".", ".."):
                raise ValueError(f"invalid staged image name: {image!r}")
            source = stage / image
            if source.is_symlink():
                raise ValueError(f"staged FrostIDE ELF must be a regular file: {image}")
            build_id, has_debug = inspect(readelf, source)
            if not has_debug:
                print(f"ELF symbols: {image} has no debug sections")
                continue
            if not build_id:
                raise ValueError(f"missing GNU build ID for {image}")
            debug_name = image + ".debug"
            debug_file, stripped = temporary / debug_name, temporary / image
            subprocess.run([objcopy, "--only-keep-debug", source, debug_file], check=True)
            subprocess.run([objcopy, "--strip-debug", f"--add-gnu-debuglink={debug_file}", source, stripped], check=True)
            if inspect(readelf, debug_file) != (build_id, True) or inspect(readelf, stripped) != (build_id, False):
                raise ValueError(f"split debug files do not preserve the build ID and DWARF partition for {image}")
            link = subprocess.check_output([readelf, "--string-dump=.gnu_debuglink", stripped], text=True)
            if not re.search(r"\[\s*0\]\s+" + re.escape(debug_name) + r"\s*$", link, re.M):
                raise ValueError(f"wrong GNU debug link for {image}")
            stripped.chmod(stat.S_IMODE(source.stat().st_mode))
            modules.append({
                "image": image,
                "debug": debug_name,
                "buildId": build_id,
                "imageSha256": digest(stripped),
                "debugSha256": digest(debug_file),
            })
            print(f"ELF symbols: {image} + {debug_name}, GNU build ID {build_id}")

        if not modules:
            raise ValueError("no staged FrostIDE ELF images contain debug sections")
        pending = temporary / "symbols.zip"
        with zipfile.ZipFile(pending, "w", compression=zipfile.ZIP_DEFLATED) as bundle:
            bundle.writestr("symbols.json", json.dumps({"schema": SCHEMA, "modules": modules}, indent=2) + "\n")
            for module in modules:
                for key in ("image", "debug"):
                    bundle.write(temporary / module[key], module[key])
        try:
            for module in modules:
                name = module["image"]
                source = stage / name
                backup = temporary / (name + ".original")
                os.link(source, backup)
                os.replace(temporary / name, source)
                changed.append((source, backup))
            os.replace(pending, archive)
        except OSError:
            for source, backup in reversed(changed):
                os.replace(backup, source)
            raise
        print(f"ELF symbols: {archive} ({len(modules)} modules)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--llvm-bin", type=Path, required=True)
    parser.add_argument("--replace", action="store_true")
    parser.add_argument("images", nargs="+")
    args = parser.parse_args()
    try:
        split_stage(args.stage, args.output, args.llvm_bin, args.images, args.replace)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"ELF symbols: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
