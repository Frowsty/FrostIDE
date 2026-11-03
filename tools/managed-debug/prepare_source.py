#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Prepare, but do not build or install, pinned netcoredbg source in a new owned directory.
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path, PurePosixPath

HERE = Path(__file__).resolve().parent


def run(argv: list[str], *, cwd: Path | None = None) -> str:
    result = subprocess.run(argv, cwd=cwd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return result.stdout.strip()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--runtime-checkout", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="new directory owned by this preparation")
    args = parser.parse_args()
    manifest = json.loads((HERE / "inputs.json").read_text(encoding="utf-8-sig"))
    git = shutil.which("git")
    if not git:
        raise ValueError("git was not found")
    runtime = args.runtime_checkout.resolve(strict=True)
    if Path(run([git, "rev-parse", "--show-toplevel"], cwd=runtime)).resolve() != runtime:
        raise ValueError("runtime path must be the checkout root")
    if run([git, "rev-parse", "HEAD"], cwd=runtime) != manifest["runtimeCommit"]:
        raise ValueError("runtime checkout is not at the pinned commit")
    if run([git, "status", "--porcelain", "--untracked-files=all", "--ignored"], cwd=runtime):
        raise ValueError("runtime checkout must have no changes, untracked or ignored files")
    subprocess.run([sys.executable, str(HERE / "audit_inputs.py"), "--cache", str(args.cache)], check=True)
    source = args.cache / manifest["netcoredbg"]["archive"]
    output = args.output.resolve()
    output.mkdir(parents=False, exist_ok=False)
    prefix = "netcoredbg-" + manifest["netcoredbg"]["commit"]
    with tarfile.open(source, "r:gz") as archive:
        members = archive.getmembers()
        for member in members:
            path = PurePosixPath(member.name)
            if not path.parts or path.parts[0] != prefix or ".." in path.parts or not (member.isdir() or member.isfile()):
                raise ValueError(f"unexpected source archive entry: {member.name}")
        archive.extractall(output, members=members, filter="data")
    tree = output / prefix
    patch = HERE / "dap-only.patch"
    # The verified upstream archive contains both CRLF and LF files. Git on Unix
    # otherwise rejects LF patch context against CRLF source lines.
    run([git, "apply", "--ignore-space-change", "--check", str(patch)], cwd=tree)
    run([git, "apply", "--ignore-space-change", str(patch)], cwd=tree)
    shutil.copyfile(HERE / "packages.lock.json", tree / "src" / "managed" / "packages.lock.json")
    print(f"Prepared {tree}; runtime checkout {runtime}. No build or installation was performed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, tarfile.TarError, subprocess.CalledProcessError) as error:
        print(f"managed-debug preparation: {error}", file=sys.stderr)
        raise SystemExit(1)
