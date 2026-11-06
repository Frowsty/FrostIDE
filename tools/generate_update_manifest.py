#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# Generate signed FrostIDE updater manifests (.json) and detached Ed25519 signatures (.json.sig).
"""Generate signed FrostIDE updater manifests (.json) and detached Ed25519 signatures (.json.sig)."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import secrets
import shutil
from pathlib import Path

DEFAULT_EXPIRES_UNIX = 1924991999  # 2030-12-31T23:59:59Z

# --- RFC 8032 Ed25519 (pure-Python fallback so no third-party wheel is required) ---

_Q = 2**255 - 19
_L = 2**252 + 27742317777372353535851937790883648493
_D = (-121665 * pow(121666, _Q - 2, _Q)) % _Q
_I = pow(2, (_Q - 1) // 4, _Q)


def _xrecover(y: int) -> int:
    xx = (y * y - 1) * pow(_D * y * y + 1, _Q - 2, _Q) % _Q
    x = pow(xx, (_Q + 3) // 8, _Q)
    if (x * x - xx) % _Q != 0:
        x = (x * _I) % _Q
    if x % 2 != 0:
        x = _Q - x
    return x


_BY = (4 * pow(5, _Q - 2, _Q)) % _Q
_BX = _xrecover(_BY)
_B = (_BX % _Q, _BY % _Q, 1, (_BX * _BY) % _Q)


def _edwards_add(p: tuple[int, int, int, int], q: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
    x1, y1, z1, t1 = p
    x2, y2, z2, t2 = q
    a = (y1 - x1) * (y2 - x2) % _Q
    b = (y1 + x1) * (y2 + x2) % _Q
    c = t1 * 2 * _D * t2 % _Q
    dd = z1 * 2 * z2 % _Q
    e = (b - a) % _Q
    f = (dd - c) % _Q
    g = (dd + c) % _Q
    h = (b + a) % _Q
    return (e * f % _Q, g * h % _Q, f * g % _Q, e * h % _Q)


def _scalarmult(p: tuple[int, int, int, int], e: int) -> tuple[int, int, int, int]:
    res = (0, 1, 1, 0)
    cur = p
    while e > 0:
        if e & 1:
            res = _edwards_add(res, cur)
        cur = _edwards_add(cur, cur)
        e >>= 1
    return res


def _encodepoint(p: tuple[int, int, int, int]) -> bytes:
    x, y, z, _ = p
    zi = pow(z, _Q - 2, _Q)
    x = (x * zi) % _Q
    y = (y * zi) % _Q
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


def _decodepoint(s: bytes) -> tuple[int, int, int, int]:
    if len(s) != 32:
        raise ValueError("Invalid Ed25519 point length")
    unclamped = int.from_bytes(s, "little")
    y = unclamped & ((1 << 255) - 1)
    x_sign = unclamped >> 255
    if y >= _Q:
        raise ValueError("Point y out of range")
    x = _xrecover(y)
    if (x & 1) != x_sign:
        x = _Q - x
    return (x, y, 1, (x * y) % _Q)


def ed25519_public_key(seed: bytes) -> bytes:
    if len(seed) != 32:
        raise ValueError("Ed25519 seed must be 32 bytes")
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return _encodepoint(_scalarmult(_B, a))


def ed25519_sign(seed: bytes, message: bytes) -> bytes:
    if len(seed) != 32:
        raise ValueError("Ed25519 seed must be 32 bytes")
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    prefix = h[32:]
    pub = _encodepoint(_scalarmult(_B, a))
    r = int.from_bytes(hashlib.sha512(prefix + message).digest(), "little") % _L
    r_point = _encodepoint(_scalarmult(_B, r))
    k = int.from_bytes(hashlib.sha512(r_point + pub + message).digest(), "little") % _L
    s = (r + k * a) % _L
    return r_point + s.to_bytes(32, "little")


def ed25519_verify(public_key: bytes, signature: bytes, message: bytes) -> None:
    if len(public_key) != 32 or len(signature) != 64:
        raise ValueError("Invalid Ed25519 key or signature size")
    r_bytes = signature[:32]
    s = int.from_bytes(signature[32:], "little")
    if s >= _L:
        raise ValueError("Ed25519 signature scalar out of range")
    a_point = _decodepoint(public_key)
    r_point = _decodepoint(r_bytes)
    k = int.from_bytes(hashlib.sha512(r_bytes + public_key + message).digest(), "little") % _L
    lhs = _encodepoint(_scalarmult(_B, s))
    rhs = _encodepoint(_edwards_add(r_point, _scalarmult(a_point, k)))
    if lhs != rhs:
        raise ValueError("Ed25519 signature verification failed")


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


def resolve_signing_seed(repo_root: Path, config_arg: Path | None, seed_arg: str | None) -> tuple[bytes, str | None]:
    if seed_arg:
        return bytes.fromhex(seed_arg.strip()), None
    env_seed = os.environ.get("FROST_SIGNING_SEED_HEX", "").strip()
    if env_seed:
        return bytes.fromhex(env_seed), None

    candidates: list[Path] = []
    if config_arg is not None:
        candidates.append(config_arg)
    env_cfg = os.environ.get("FROST_SIGNING_CONFIG", "").strip()
    if env_cfg:
        candidates.append(Path(env_cfg))
    candidates.extend(
        [
            repo_root / "signing.config.json",
            Path("E:/FrostBuilds/signing/release-signing.json"),
            Path.home() / ".frost" / "release-signing.json",
        ]
    )

    for candidate in candidates:
        if candidate.is_file():
            cfg = json.loads(candidate.read_text(encoding="utf-8"))
            seed_hex = str(cfg.get("seed_hex", "")).strip()
            if not seed_hex:
                raise ValueError(f"Signing config '{candidate}' is missing 'seed_hex'")
            repo_override = cfg.get("repo")
            return bytes.fromhex(seed_hex), (str(repo_override) if repo_override else None)

    searched = ", ".join(str(p) for p in candidates)
    raise FileNotFoundError(
        f"No signing config file found (searched: {searched}). "
        "Pass --config <path>, set FROST_SIGNING_CONFIG, or generate one with --generate-config <path>."
    )


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
    parser.add_argument(
        "--generate-config",
        type=Path,
        default=None,
        help="Generate a new Ed25519 keypair and write it to the specified JSON config file, then exit",
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=None,
        help="Path to JSON signing config file containing 'seed_hex' (default: signing.config.json or E:/FrostBuilds/signing/release-signing.json)",
    )
    parser.add_argument("--version", default=default_version, help="Release version (default: from version.json)")
    parser.add_argument("--build", type=int, default=default_build, help="Monotonic integer build number")
    parser.add_argument("--tag", default=None, help="GitHub release tag (default: v<version>)")
    parser.add_argument("--repo", default=None, help="GitHub owner/repo (default: Frowsty/FrostIDE)")
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
        default=None,
        help="Output directory for the manifest .json, .json.sig, and .zip files",
    )
    parser.add_argument(
        "--seed-hex",
        default=None,
        help="Optional 64-character lowercase hex Ed25519 private key seed override (32 bytes)",
    )
    args = parser.parse_args()

    if args.generate_config is not None:
        cfg_path = args.generate_config.resolve()
        cfg_path.parent.mkdir(parents=True, exist_ok=True)
        seed = secrets.token_bytes(32)
        pub_hex = ed25519_public_key(seed).hex()
        payload = {
            "seed_hex": seed.hex(),
            "public_key_hex": pub_hex,
            "repo": args.repo or "Frowsty/FrostIDE",
        }
        cfg_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        print(f"Wrote new signing config to: {cfg_path}")
        print(f"Public key hex (embed in workbench.cpp / FROST_UPDATE_PUBLIC_KEY_HEX): {pub_hex}")
        return 0

    if args.out_dir is None:
        parser.error("--out-dir is required when generating release manifests")

    seed_bytes, cfg_repo = resolve_signing_seed(repo_root, args.config, args.seed_hex)
    if len(seed_bytes) != 32:
        raise ValueError("Ed25519 seed must be 64 hex characters (32 bytes)")
    public_key = ed25519_public_key(seed_bytes)
    public_key_hex = public_key.hex()
    repo_name = args.repo or cfg_repo or "Frowsty/FrostIDE"

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
        # Package files at the root of the zip (no top-level wrapper folder) so in-place
        # Expand-Archive overwrites frost.exe and frost_app.dll directly in the target directory.
        shutil.make_archive(base_name, "zip", root_dir=str(stage_dir))
    elif not target_zip.is_file():
        raise ValueError("Provide either --zip <path> or --stage-dir <dir> (or place the zip in --out-dir).")

    asset_size = target_zip.stat().st_size
    asset_sha256 = compute_sha256(target_zip)
    asset_url = f"https://github.com/{repo_name}/releases/download/{tag}/{target_zip.name}"

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
        signature = ed25519_sign(seed_bytes, manifest_bytes)
        ed25519_verify(public_key, signature, manifest_bytes)

        manifest_path.write_bytes(manifest_bytes)
        sig_path.write_bytes(signature)
        print(f"Wrote   : {manifest_path.name} ({len(manifest_bytes)} B) + {sig_path.name} ({len(signature)} B)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
