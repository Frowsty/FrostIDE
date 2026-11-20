# SPDX-License-Identifier: Apache-2.0
# Registers an extracted Linux stage as a per-user desktop application and MIME handler.
"""Register or unregister this extracted FrostIDE stage for the current Linux user."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

DESKTOP = "applications/org.frostide.FrostIDE.desktop"
MIME_PACKAGE = "mime/packages/org.frostide.FrostIDE.xml"
RECEIPT = "frostide/shell-registration.json"
MIME_TYPES = (
    ("application/x-frostide-project", "FrostIDE project", "*.frostproj", "application/json"),
    ("text/x-ms-sln", "Visual Studio solution", "*.sln", "text/plain"),
    ("application/x-ms-slnx", "Visual Studio XML solution", "*.slnx", "application/xml"),
)


def exec_argument(path):
    value = str(path)
    if any(ord(char) < 32 or ord(char) == 127 for char in value):
        raise ValueError("the FrostIDE launcher path contains a control character")
    # Escape the field-code marker and then apply both Desktop Entry quoting layers.
    value = value.replace("%", "%%")
    quoted = '"' + "".join("\\" + char if char in '\\"$`' else char for char in value) + '"'
    return quoted.replace("\\", "\\\\")


def registration_files(launcher):
    types = [kind for kind, _, _, _ in MIME_TYPES] + ["text/x-cmake", "text/x-lua"]
    # Bash runs the staged wrapper directly as a script. No command string or filename is evaluated.
    content = (
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=FrostIDE\n"
        f"Exec=/bin/bash -- {exec_argument(launcher)} -- %F\n"
        "Terminal=false\n"
        "Categories=Development;IDE;\n"
        f"MimeType={';'.join(types)};\n"
    )
    xml = ['<?xml version="1.0" encoding="UTF-8"?>',
           '<mime-info xmlns="http://www.freedesktop.org/standards/shared-mime-info">']
    for kind, label, pattern, parent in MIME_TYPES:
        xml.extend((f'  <mime-type type="{kind}">', f'    <comment>{label}</comment>',
                    f'    <sub-class-of type="{parent}"/>',
                    f'    <glob pattern="{pattern}" weight="10"/>', '  </mime-type>'))
    xml.append("</mime-info>")
    return {DESKTOP: content.encode("utf-8"), MIME_PACKAGE: ("\n".join(xml) + "\n").encode("utf-8")}


def digest(content):
    return hashlib.sha256(content).hexdigest()


def current_digest(path):
    if path.is_symlink() or not path.is_file():
        return None
    return digest(path.read_bytes())


def target(root, relative):
    path = root / relative
    # An explicitly selected XDG root may be a symlink. Never follow symlinks beneath it.
    for parent in path.parents:
        if parent == root:
            break
        if parent.is_symlink():
            raise ValueError(f"registration directory is a symlink: {parent}")
    return path


def publish_file(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as file:
            file.write(content)
            os.fchmod(file.fileno(), 0o644)
        os.link(temporary, path)
    finally:
        os.unlink(temporary)


def update_commands(root):
    commands = []
    for name, directory in (("update-mime-database", "mime"), ("update-desktop-database", "applications")):
        program = shutil.which(name)
        if program is None:
            raise ValueError(f"{name} is required for per-user registration")
        commands.append([program, str(root / directory)])
    return commands


def refresh(commands):
    for command in commands:
        subprocess.run(command, check=True)


def register(root, launcher):
    files = registration_files(launcher)
    hashes = {name: digest(content) for name, content in files.items()}
    receipt = target(root, RECEIPT)
    record = {"schema": 1, "launcher": str(launcher), "files": hashes}
    commands = update_commands(root)
    if receipt.is_file() and not receipt.is_symlink():
        if json.loads(receipt.read_text(encoding="utf-8")) == record and all(
            current_digest(target(root, name)) == expected for name, expected in hashes.items()
        ):
            refresh(commands)
            return
    for name in (*files, RECEIPT):
        path = target(root, name)
        if os.path.lexists(path):
            raise ValueError(f"registration file already exists: {path}")
    created = []
    try:
        for name, content in files.items():
            path = target(root, name)
            publish_file(path, content)
            created.append((path, hashes[name]))
        refresh(commands)
        publish_file(receipt, (json.dumps(record, indent=2) + "\n").encode("utf-8"))
    except (OSError, subprocess.CalledProcessError):
        for path, expected in reversed(created):
            if current_digest(path) == expected:
                path.unlink()
        if len(created) == len(files):
            try:
                refresh(commands)
            except (OSError, subprocess.CalledProcessError):
                pass
        raise


def unregister(root, launcher):
    receipt = target(root, RECEIPT)
    if not os.path.lexists(receipt):
        return
    if receipt.is_symlink() or not receipt.is_file():
        raise ValueError(f"invalid desktop registration receipt: {receipt}")
    record = json.loads(receipt.read_text(encoding="utf-8"))
    if not isinstance(record, dict) or record.get("schema") != 1 or record.get("launcher") != str(launcher):
        raise ValueError("desktop registration belongs to another FrostIDE stage")
    expected = record.get("files")
    if not isinstance(expected, dict) or set(expected) != {DESKTOP, MIME_PACKAGE} or any(
        not isinstance(item, str) or len(item) != 64 or any(char not in "0123456789abcdef" for char in item)
        for item in expected.values()
    ):
        raise ValueError("invalid desktop registration receipt")
    commands = update_commands(root)
    paths = {name: target(root, name) for name in expected}
    for command in commands:
        Path(command[1]).mkdir(parents=True, exist_ok=True)
    for name, path in paths.items():
        if current_digest(path) == expected[name]:
            path.unlink()
        elif os.path.lexists(path):
            print(f"Preserved modified file: {path}")
    refresh(commands)
    receipt.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("register", "unregister"))
    args = parser.parse_args()
    if not sys.platform.startswith("linux"):
        parser.error("Linux desktop registration requires Linux")
    launcher = Path(__file__).resolve().with_name("frost")
    data = Path(os.environ.get("XDG_DATA_HOME", ""))
    if not data.is_absolute():
        data = Path.home() / ".local" / "share"
    data = data.resolve()
    if args.action == "register":
        if not os.access(launcher, os.X_OK) or not launcher.is_file() or not launcher.with_name("frost.bin").is_file():
            raise ValueError("register from a complete staged FrostIDE archive")
        register(data, launcher)
    else:
        unregister(data, launcher)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"desktop registration: {error}", file=sys.stderr)
        sys.exit(1)
