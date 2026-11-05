#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# Generate signed FrostIDE updater manifests (.json) and detached Ed25519 signatures (.json.sig).
"""Generate signed FrostIDE updater manifests (.json) and detached Ed25519 signatures (.json.sig)."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

# Default Ed25519 seed matching the built-in fallback public key in src/app/src/workbench.cpp:
# Public key: d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a
DEFAULT_SEED_HEX = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"
DEFAULT_EXPIRES_UNIX = 1924991999  # 2030-12-31T23:59:59Z


def compute_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().lower()


def build_manifest_bytes(
    *,
    channel: str,
    platform: str,
    arch: str,
    version: str,
    build: int,
    expires: int,
    asset_url: str,
    asset_size: int,
    asset_sha256: str,
) -> bytes:
    # Note: stable channel requires a non-prerelease version string (no "-beta" suffix).
    manifest_version = re.sub(r"[-+].*$", "", version) if channel == "stable" else version
    payload = {
        "schema": 1,
        "channel": channel,
        "platform": platform,
        "arch": arch,
        "version": manifest_version,
        "build": build,
        "expires": expires,
        "asset": {
            "url": asset_url,
            "size": asset_size,
            "sha256": asset_sha256,
        },
    }
    return json.dumps(payload, indent=2).encode("utf-8")


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    version_json_path = repo_root / "version.json"
    default_version = "2026.11.1.0-beta"
    default_build = 15
    if version_json_path.is_file():
        data = json.loads(version_json_path.read_text(encoding="utf-8"))
        default_version = str(data.get("version", default_version))
        default_build = int(data.get("buildNumber", default_build))

    parser = argparse.ArgumentParser(
        description="Generate signed FrostIDE update manifests and .sig files for a GitHub Release."
    )
    parser.add_argument("--version", default=default_version, help="Release version (default: from version.json)")
    parser.add_argument("--build", type=int, default=default_build, help="Monotonic integer build number")
    parser.add_argument("--tag", default=None, help="GitHub release tag (default: v<version>)")
    parser.add_argument("--repo", default="Frowsty/FrostIDE", help="GitHub owner/repo (default: Frowsty/FrostIDE)")
    parser.add_argument("--platform", default="windows", choices=["windows", "linux"], help="Target platform")
    parser.add_argument("--arch", default="x64", choices=["x64", "arm64"], help="Target architecture")
    parser.add_argument("--expires", type=int, default=DEFAULT_EXPIRES_UNIX, help="Manifest expiration Unix timestamp")
    parser.add_argument("--zip", type=Path, default=None, help="Path to existing release .zip archive")
    parser.add_argument(
        "--stage-dir",
        type=Path,
        default=None,
        help="Path to staged folder (e.g. E:\\FrostBuilds\\beta\\FrostIDE-2026.11.1.0-beta) to zip if --zip is omitted",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        required=True,
        help="Output directory for the manifest .json, .json.sig, and .zip files",
    )
    parser.add_argument(
        "--seed-hex",
        default=DEFAULT_SEED_HEX,
        help="64-character lowercase hex Ed25519 private key seed (32 bytes)",
    )
    args = parser.parse_args()

    out_dir: Path = args.out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    tag = args.tag or f"v{args.version}"
    zip_filename = f"FrostIDE-{args.version}.zip"
    target_zip = out_dir / zip_filename

    if args.zip is not None:
        src_zip = args.zip.resolve()
        if not src_zip.is_file():
            raise FileNotFoundError(f"Zip file not found: {src_zip}")
        if src_zip != target_zip:
            shutil.copy2(src_zip, target_zip)
    elif args.stage_dir is not None:
        stage_dir = args.stage_dir.resolve()
        if not stage_dir.is_dir():
            raise FileNotFoundError(f"Stage directory not found: {stage_dir}")
        base_name = str(target_zip.with_suffix(""))
        shutil.make_archive(base_name, "zip", root_dir=str(stage_dir.parent), base_dir=stage_dir.name)
    elif not target_zip.is_file():
        raise ValueError("Provide either --zip <path> or --stage-dir <dir> (or place the zip in --out-dir).")

    asset_size = target_zip.stat().st_size
    asset_sha256 = compute_sha256(target_zip)
    asset_url = f"https://github.com/{args.repo}/releases/download/{tag}/{target_zip.name}"

    seed_bytes = bytes.fromhex(args.seed_hex)
    if len(seed_bytes) != 32:
        raise ValueError("--seed-hex must be 64 hex characters (32 bytes)")
    private_key = Ed25519PrivateKey.from_private_bytes(seed_bytes)
    public_key = private_key.public_key()
    public_key_hex = public_key.public_bytes_raw().hex()

    print(f"Archive : {target_zip}")
    print(f"Size    : {asset_size} bytes")
    print(f"SHA-256 : {asset_sha256}")
    print(f"URL     : {asset_url}")
    print(f"PubKey  : {public_key_hex}")

    for channel in ("stable", "eap"):
        manifest_name = f"frost-update-{channel}-{args.platform}-{args.arch}.json"
        sig_name = f"{manifest_name}.sig"
        manifest_path = out_dir / manifest_name
        sig_path = out_dir / sig_name

        manifest_bytes = build_manifest_bytes(
            channel=channel,
            platform=args.platform,
            arch=args.arch,
            version=args.version,
            build=args.build,
            expires=args.expires,
            asset_url=asset_url,
            asset_size=asset_size,
            asset_sha256=asset_sha256,
        )
        signature = private_key.sign(manifest_bytes)
        public_key.verify(signature, manifest_bytes)

        manifest_path.write_bytes(manifest_bytes)
        sig_path.write_bytes(signature)
        print(f"Wrote   : {manifest_path.name} ({len(manifest_bytes)} B) + {sig_path.name} ({len(signature)} B)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
