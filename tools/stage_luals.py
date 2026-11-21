# SPDX-License-Identifier: Apache-2.0
# Stages the exact pinned LuaLS distribution, independent of generated SDK runtime files.

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import stat
import sys
import tarfile
import tempfile
import zipfile


def stage_luals(repo: Path, sdk: Path, platform: str, destination: Path | None) -> None:
    pin = json.loads((repo / "sdk/sdk.lock.json").read_text(encoding="utf-8"))[platform]["luals"]
    archive = sdk / "_downloads" / pin["file"]
    if not archive.is_file():
        raise ValueError(f"missing bundled LuaLS archive: {archive}")

    windows = platform == "windows"
    executable = "bin/lua-language-server.exe" if windows else "bin/lua-language-server"
    with archive.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != pin["sha256"]:
            raise ValueError("bundled LuaLS archive SHA-256 does not match sdk.lock")
        stream.seek(0)
        bundle = zipfile.ZipFile(stream) if windows else tarfile.open(fileobj=stream)
        with bundle:
            inventory = {}
            members = bundle.infolist() if windows else bundle
            for member in members:
                if windows:
                    name, mode, is_directory = member.filename, member.external_attr >> 16, member.is_dir()
                    if stat.S_IFMT(mode) not in (0, stat.S_IFDIR if is_directory else stat.S_IFREG):
                        raise ValueError(f"unsupported LuaLS archive entry: {name}")
                else:
                    name, mode, is_directory = member.name, member.mode, member.isdir()
                    if not is_directory and not member.isfile():
                        raise ValueError(f"unsupported LuaLS archive entry: {name}")
                path = PurePosixPath(name)
                if path.is_absolute() or ".." in path.parts or "\\" in name or ":" in name:
                    raise ValueError(f"unsafe LuaLS archive path: {name}")
                if not path.parts and is_directory:
                    continue  # A tarball may contain its root directory entry.
                if not path.parts or any(part.endswith((".", " ")) for part in path.parts):
                    raise ValueError(f"unsafe LuaLS archive path: {name}")
                key = path.as_posix().casefold() if windows else path.as_posix()
                if key in inventory:
                    raise ValueError(f"duplicate LuaLS archive path: {name}")
                inventory[key] = (path, mode, is_directory, member)

            for required in (executable, "main.lua", "LICENSE"):
                key = required.casefold() if windows else required
                if key not in inventory or inventory[key][2]:
                    raise ValueError(f"missing bundled LuaLS file in pinned archive: {required}")
            for path, _, _, _ in inventory.values():
                for parent in path.parents:
                    name = parent.as_posix()
                    entry = inventory.get(name.casefold() if windows else name)
                    if entry is not None and not entry[2]:
                        raise ValueError(f"LuaLS archive file is also a parent directory: {parent}")
            if destination is None:
                return

            if destination.exists() or destination.is_symlink():
                raise FileExistsError(f"LuaLS destination already exists: {destination}")
            with tempfile.TemporaryDirectory(prefix=".luals-stage-", dir=destination.parent) as scratch:
                payload = Path(scratch) / "payload"
                payload.mkdir()
                for path, mode, is_directory, member in inventory.values():
                    target = payload / path
                    if is_directory:
                        target.mkdir(parents=True, exist_ok=True)
                        continue
                    target.parent.mkdir(parents=True, exist_ok=True)
                    source = bundle.open(member) if windows else bundle.extractfile(member)
                    with source, target.open("xb") as output:
                        shutil.copyfileobj(source, output)
                    if not windows:
                        target.chmod(mode & 0o777)
                payload.rename(destination)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--platform", choices=("windows", "linux"), required=True)
    parser.add_argument("--destination", type=Path)
    args = parser.parse_args()
    try:
        stage_luals(args.repo, args.sdk, args.platform, args.destination)
    except (OSError, ValueError, tarfile.TarError, zipfile.BadZipFile) as error:
        print(f"LuaLS stage: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
