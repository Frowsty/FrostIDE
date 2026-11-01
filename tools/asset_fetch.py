#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# asset_fetch.py: fetches and verifies the pinned third-party assets that FrostIDE bundles
# (the Inter and JetBrains Mono fonts and the Lucide stopgap icons), as listed in
# resources/assets.lock.json (ADR 0015, PLAN.md section 2 "Icons and fonts" and risk L7).
#
#   python tools/asset_fetch.py [--repo <path>] [--manifest <file>] [--verify]
#   python tools/asset_fetch.py ... --fetch [--only <id>]... [--download-dir <dir>]
#   python tools/asset_fetch.py ... --pin --only <id> [--download-dir <dir>]
#   python tools/asset_fetch.py ... --list --only <id> [--download-dir <dir>]
#
# Modes:
#   --verify  (the default; offline) every file the manifest lists exists under its asset's
#             `dest` directory with the pinned SHA-256. An unpinned (empty) hash fails.
#   --fetch   downloads each selected asset's archive into a fresh, empty directory outside
#             the repository (<download-dir>/<id>), checks the archive's SHA-256 before the
#             archive is opened, extracts only the listed members by exact name (refusing
#             duplicate member names and members larger than maxMemberBytes before reading
#             them), checks each member's pinned SHA-256, fetches the extraFiles (licence
#             files that are not in the archive) the same way, and only when everything
#             verified writes the files into `dest`, each atomically (a temporary file and a
#             rename) and byte-exact. Destination names come from the manifest's `path`,
#             which must be a basename; archive member names are never used as paths.
#   --pin     first-time pinning of one asset: like --fetch without writing `dest`, it fills
#             in the manifest's empty sha256 fields from the download (pinned values must
#             still match), compares the archive with the GitHub release asset's published
#             digest when the URL is a GitHub release download that has one, and compares
#             each member with a second copy: the file's `crossCheckUrl`, or the asset's
#             `crossCheckBase` (the tagged repository tree) followed by the member name.
#             Rewrites the manifest.
#   --list    prints the archive's members (size and name), for choosing `member` names.
#
# Network use: only --fetch, --pin and --list download anything (urllib, https only, and
# redirects are followed only to https URLs; the test-only --allow-file-urls also accepts
# file:// URLs). Every download is limited to MAX_DOWNLOAD_BYTES while it streams in.
# --fetch refuses an asset with any unpinned (empty) hash. Downloaded data is never executed.
#
# Exit status: 0 success; 1 a verification, download or extraction failure; 2 a usage error
# or an invalid manifest. Python 3.11+, standard library only; imports nothing from the
# sibling tools.

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile
import zlib
from dataclasses import dataclass
from pathlib import Path

MANIFEST_FILE = "resources/assets.lock.json"
# Members larger than this (bytes) are refused unless the asset sets maxMemberBytes.
DEFAULT_MAX_MEMBER_BYTES = 16 * 1024 * 1024
# Download timeout per request, seconds.
DOWNLOAD_TIMEOUT_S = 300
# Downloads larger than this (bytes) are refused while they stream in: archives, cross-check
# copies and extra files. The largest pinned archive, Inter 4.1, is 33.7 MB.
MAX_DOWNLOAD_BYTES = 128 * 1024 * 1024
# The GitHub release API reply is read into memory; longer replies are refused.
MAX_API_REPLY_BYTES = 8 * 1024 * 1024
REQUIRED_FIELDS = ("id", "name", "version", "url", "archive", "sha256", "crossCheck", "licence", "copyright",
                   "reservedFontNames", "modified", "homepage", "dest", "licenceFile", "files", "adr")
_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_ID = re.compile(r"^[a-z0-9][a-z0-9-]{0,63}$")
_GITHUB_RELEASE = re.compile(r"^https://github\.com/([^/]+)/([^/]+)/releases/download/([^/]+)/([^/]+)$")
# Windows device names, which name a device rather than a file with any extension and in any case.
_WINDOWS_DEVICE = re.compile(r"^(con|prn|aux|nul|com[0-9\u00b9\u00b2\u00b3]|lpt[0-9\u00b9\u00b2\u00b3])(\..*)?$",
                             re.IGNORECASE)
# What a malformed or unsupported archive can raise from zipfile and the decompressors.
_ARCHIVE_ERRORS: tuple[type[BaseException], ...] = (zipfile.BadZipFile, zipfile.LargeZipFile, OSError,
                                                    RuntimeError, EOFError, NotImplementedError, ValueError,
                                                    zlib.error)
try:
    import lzma

    _ARCHIVE_ERRORS += (lzma.LZMAError,)
except ImportError:  # zipfile then refuses LZMA members with RuntimeError
    pass


class ManifestError(ValueError):
    """Raised for a manifest that does not match the schema (exit status 2)."""


class FetchError(RuntimeError):
    """Raised for a download, hash or extraction failure (exit status 1)."""


def is_basename(name: object) -> bool:
    """
    True when `name` is a plain file name that every supported file system stores as written:
    no separators, drive colons, NULs, '.' or '..', no leading or trailing white space, no
    trailing '.' (Windows drops it) and no Windows device name (CON, NUL, COM1.txt, ...).
    """
    if not isinstance(name, str) or not name or name in (".", ".."):
        return False
    if any(c in name for c in "/\\:\0") or name != name.strip() or name.endswith("."):
        return False
    return not _WINDOWS_DEVICE.match(name)


def _safe_member(name: object) -> bool:
    """True for an archive member name that is relative and has no '..' component or backslash."""
    if not isinstance(name, str) or not name or name.startswith("/") or "\\" in name or "\0" in name:
        return False
    return all(part not in ("", ".", "..") for part in name.split("/")) and ":" not in name


def _safe_dest(dest: object) -> bool:
    """True for a POSIX directory path below resources/ whose every component is a basename."""
    if not _safe_member(dest):
        return False
    parts = dest.split("/")
    return len(parts) >= 2 and parts[0] == "resources" and all(is_basename(part) for part in parts)


def _url_ok(url: object, allow_file_urls: bool) -> bool:
    if not isinstance(url, str):
        return False
    scheme = urllib.parse.urlsplit(url).scheme
    return scheme == "https" or (allow_file_urls and scheme == "file")


def _hash_ok(value: object) -> bool:
    return isinstance(value, str) and (value == "" or bool(_SHA256.match(value)))


def validate_manifest(document: object, where: str, allow_file_urls: bool = False) -> list[dict]:
    """
    Checks the manifest schema (resources/assets.lock.json, schemaVersion 1) and returns its
    asset list. Raises ManifestError naming the first problem. Pure.
    """
    if not isinstance(document, dict) or document.get("schemaVersion") != 1:
        raise ManifestError(f"{where}: schemaVersion must be 1")
    assets = document.get("assets")
    if not isinstance(assets, list) or not assets:
        raise ManifestError(f"{where}: 'assets' must be a non-empty list")
    seen: set[str] = set()
    for asset in assets:
        if not isinstance(asset, dict):
            raise ManifestError(f"{where}: every asset must be an object")
        label = f"{where}: {asset.get('id', '?')}"
        missing = [k for k in REQUIRED_FIELDS if k not in asset]
        if missing:
            raise ManifestError(f"{label}: lacks {', '.join(missing)}")
        if not isinstance(asset["id"], str) or not _ID.match(asset["id"]):
            raise ManifestError(f"{label}: invalid id")
        if asset["id"] in seen:
            raise ManifestError(f"{label}: duplicate id")
        seen.add(asset["id"])
        if not _url_ok(asset["url"], allow_file_urls):
            raise ManifestError(f"{label}: url must be https")
        if not is_basename(asset["archive"]) or not asset["archive"].endswith(".zip"):
            raise ManifestError(f"{label}: archive must be the basename of a .zip")
        if not _hash_ok(asset["sha256"]):
            raise ManifestError(f"{label}: sha256 must be empty or 64 lower-case hex digits")
        for key in ("name", "version", "crossCheck", "licence", "homepage", "adr"):
            if not isinstance(asset[key], str) or not asset[key]:
                raise ManifestError(f"{label}: {key} must be a non-empty string")
        if not isinstance(asset["copyright"], list) or not asset["copyright"] or \
                not all(isinstance(c, str) and c for c in asset["copyright"]):
            raise ManifestError(f"{label}: copyright must be a non-empty list of strings")
        if not isinstance(asset["reservedFontNames"], list) or \
                not all(isinstance(n, str) for n in asset["reservedFontNames"]):
            raise ManifestError(f"{label}: reservedFontNames must be a list of strings")
        if not isinstance(asset["modified"], bool):
            raise ManifestError(f"{label}: modified must be true or false")
        if not _safe_dest(asset["dest"]):
            raise ManifestError(f"{label}: dest must be a directory below resources/")
        if not _safe_member(asset["adr"]):
            raise ManifestError(f"{label}: adr must be a repository-relative path")
        base = asset.get("crossCheckBase")
        if base is not None and (not _url_ok(base, allow_file_urls) or not base.endswith("/")):
            raise ManifestError(f"{label}: crossCheckBase must be an https URL ending in '/'")
        limit = asset.get("maxMemberBytes", DEFAULT_MAX_MEMBER_BYTES)
        if not isinstance(limit, int) or isinstance(limit, bool) or limit <= 0:
            raise ManifestError(f"{label}: maxMemberBytes must be a positive integer")
        files = asset["files"]
        if not isinstance(files, list) or not files:
            raise ManifestError(f"{label}: files must be a non-empty list")
        paths: set[str] = set()
        members: set[str] = set()
        for entry in files:
            if not isinstance(entry, dict):
                raise ManifestError(f"{label}: every file must be an object")
            if not _safe_member(entry.get("member")):
                raise ManifestError(f"{label}: member {entry.get('member')!r} must be a relative archive path "
                                    f"without '..'")
            if entry["member"] in members:
                raise ManifestError(f"{label}: member {entry['member']} listed twice")
            members.add(entry["member"])
            if not is_basename(entry.get("path")):
                raise ManifestError(f"{label}: path {entry.get('path')!r} must be a basename")
            if not _hash_ok(entry.get("sha256")):
                raise ManifestError(f"{label}: {entry['path']}: sha256 must be empty or 64 hex digits")
            if "crossCheckUrl" in entry and not _url_ok(entry["crossCheckUrl"], allow_file_urls):
                raise ManifestError(f"{label}: {entry['path']}: crossCheckUrl must be https")
            if entry["path"] in paths:
                raise ManifestError(f"{label}: path {entry['path']} listed twice")
            paths.add(entry["path"])
        extras = asset.get("extraFiles", [])
        if not isinstance(extras, list):
            raise ManifestError(f"{label}: extraFiles must be a list")
        for entry in extras:
            if not isinstance(entry, dict) or not _url_ok(entry.get("url"), allow_file_urls):
                raise ManifestError(f"{label}: every extra file needs an https url")
            if not is_basename(entry.get("path")):
                raise ManifestError(f"{label}: path {entry.get('path')!r} must be a basename")
            if not _hash_ok(entry.get("sha256")):
                raise ManifestError(f"{label}: {entry['path']}: sha256 must be empty or 64 hex digits")
            if entry["path"] in paths:
                raise ManifestError(f"{label}: path {entry['path']} listed twice")
            paths.add(entry["path"])
        if asset["licenceFile"] not in paths:
            raise ManifestError(f"{label}: licenceFile {asset['licenceFile']!r} is not one of the listed files")
    return assets


def load_manifest(path: Path, allow_file_urls: bool = False) -> tuple[dict, list[dict]]:
    """Reads and validates the manifest; returns (document, assets). Raises ManifestError."""
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise ManifestError(f"{path}: {error}") from error
    return document, validate_manifest(document, str(path), allow_file_urls)


def asset_paths(asset: dict) -> list[tuple[str, str]]:
    """(path, sha256) for every file the asset installs into its dest directory."""
    return [(f["path"], f["sha256"]) for f in asset["files"]] + \
        [(f["path"], f["sha256"]) for f in asset.get("extraFiles", [])]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify(repo: Path, assets: list[dict]) -> list[str]:
    """Offline check: every listed file exists under dest with its pinned hash. Returns problems."""
    problems = []
    for asset in assets:
        if not asset["sha256"]:
            problems.append(f"{asset['id']}: archive {asset['archive']} not pinned (empty sha256); "
                            f"run --pin --only {asset['id']}")
        for name, expected in asset_paths(asset):
            target = repo / asset["dest"] / name
            shown = f"{asset['dest']}/{name}"
            if not expected:
                problems.append(f"{shown}: not pinned (empty sha256); run --pin --only {asset['id']}")
            elif not target.is_file():
                problems.append(f"{shown}: missing; run --fetch --only {asset['id']}")
            elif sha256_file(target) != expected:
                problems.append(f"{shown}: SHA-256 differs from the pin")
    return problems


class HttpsOnlyRedirects(urllib.request.HTTPRedirectHandler):
    """Follows HTTP redirects only to https URLs; any other target raises URLError."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if urllib.parse.urlsplit(newurl).scheme != "https":
            raise urllib.error.URLError(f"refusing a {code} redirect to the non-https URL {newurl}")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


# Replaces urllib's default redirect handler (build_opener drops a default when given a subclass).
_OPENER = urllib.request.build_opener(HttpsOnlyRedirects)


def download(url: str, target: Path, limit: int = MAX_DOWNLOAD_BYTES) -> str:
    """
    Downloads `url` to `target` (a new file) and returns its SHA-256. Raises FetchError on any
    network or HTTP failure, a redirect to a non-https URL, or more than `limit` bytes (checked
    while streaming). Side effect: writes `target` (possibly partially on failure).
    """
    request = urllib.request.Request(url, headers={"User-Agent": "frostide-asset-fetch/1"})
    digest = hashlib.sha256()
    total = 0
    try:
        with _OPENER.open(request, timeout=DOWNLOAD_TIMEOUT_S) as response, target.open("xb") as out:
            for chunk in iter(lambda: response.read(1 << 20), b""):
                total += len(chunk)
                if total > limit:
                    raise FetchError(f"download of {url} exceeds the {limit}-byte limit")
                digest.update(chunk)
                out.write(chunk)
    except (urllib.error.URLError, OSError, ValueError) as error:
        raise FetchError(f"download of {url} failed: {error}") from error
    return digest.hexdigest()


def github_digest(url: str) -> str | None:
    """
    The SHA-256 GitHub publishes for a release asset (the REST API's `digest` field), or None
    when `url` is not a GitHub release download or the asset has no digest. Raises FetchError
    when the API cannot be read.
    """
    m = _GITHUB_RELEASE.match(url)
    if not m:
        return None
    owner, repo, tag, name = m.groups()
    api = f"https://api.github.com/repos/{owner}/{repo}/releases/tags/{urllib.parse.quote(tag)}"
    request = urllib.request.Request(api, headers={"User-Agent": "frostide-asset-fetch/1",
                                                   "Accept": "application/vnd.github+json"})
    try:
        with _OPENER.open(request, timeout=60) as response:
            reply = response.read(MAX_API_REPLY_BYTES + 1)
        if len(reply) > MAX_API_REPLY_BYTES:
            raise FetchError(f"the GitHub release {owner}/{repo}@{tag} reply exceeds {MAX_API_REPLY_BYTES} bytes")
        release = json.loads(reply.decode("utf-8"))
    except (urllib.error.URLError, OSError, ValueError) as error:
        raise FetchError(f"cannot read the GitHub release {owner}/{repo}@{tag}: {error}") from error
    for item in release.get("assets", []):
        if item.get("name") == urllib.parse.unquote(name):
            digest = item.get("digest")
            if isinstance(digest, str) and digest.startswith("sha256:"):
                return digest.removeprefix("sha256:").lower()
            return None
    raise FetchError(f"the GitHub release {owner}/{repo}@{tag} has no asset {name}")


def fresh_dir(base: Path, repo: Path, asset_id: str) -> Path:
    """
    Creates and returns <base>/<asset_id>, which must not exist yet or be empty and must lie
    outside the repository. Raises FetchError otherwise.
    """
    target = (base / asset_id).resolve()
    repo = repo.resolve()
    if target == repo or repo in target.parents:
        raise FetchError(f"download directory {target} is inside the repository")
    if target.exists() and (not target.is_dir() or any(target.iterdir())):
        raise FetchError(f"download directory {target} is not empty")
    target.mkdir(parents=True, exist_ok=True)
    return target


@dataclass
class Staged:
    """One verified file, ready to be written into dest."""

    path: str
    data: bytes
    sha256: str


def _check_hash(label: str, expected: str, actual: str) -> None:
    if expected and expected != actual:
        raise FetchError(f"{label}: SHA-256 {actual} differs from the pinned {expected}")


def read_members(archive: Path, asset: dict) -> tuple[list[str], dict[str, bytes]]:
    """
    Opens a verified archive and returns (every member name, {listed member: bytes}). Refuses
    duplicate member names, a missing listed member and members over maxMemberBytes (checked
    on the declared size before reading and on the bytes actually read). Raises FetchError.
    """
    limit = asset.get("maxMemberBytes", DEFAULT_MAX_MEMBER_BYTES)
    try:
        with zipfile.ZipFile(archive) as zf:
            infos = zf.infolist()
            names = [info.filename for info in infos]
            if len(set(names)) != len(names):
                duplicates = sorted({n for n in names if names.count(n) > 1})
                raise FetchError(f"{asset['archive']}: duplicate member names {', '.join(duplicates)}")
            by_name = {info.filename: info for info in infos}
            out: dict[str, bytes] = {}
            for entry in asset["files"]:
                member = entry["member"]
                info = by_name.get(member)
                if info is None or info.is_dir():
                    raise FetchError(f"{asset['archive']}: no member {member}")
                if info.file_size > limit:
                    raise FetchError(f"{asset['archive']}: {member} is {info.file_size} bytes, over the "
                                     f"{limit}-byte limit")
                with zf.open(info) as stream:
                    data = stream.read(limit + 1)
                if len(data) > limit:
                    raise FetchError(f"{asset['archive']}: {member} expands past the {limit}-byte limit")
                out[member] = data
            return names, out
    except FetchError:
        raise
    except _ARCHIVE_ERRORS as error:
        raise FetchError(f"{asset['archive']}: cannot read the archive: {error}") from error


def cross_check_url(asset: dict, entry: dict) -> str | None:
    """The second source --pin compares a member with: the file's crossCheckUrl, else the asset's
    crossCheckBase followed by the member name (a tagged repository tree), else None."""
    if "crossCheckUrl" in entry:
        return entry["crossCheckUrl"]
    base = asset.get("crossCheckBase")
    return base + entry["member"] if base else None


def stage_asset(asset: dict, work: Path, pin: bool) -> list[Staged]:
    """
    Downloads and verifies one asset into `work` and returns its staged files. With `pin`,
    empty hashes are filled into `asset` (pinned ones must still match), the GitHub digest is
    compared and cross-check URLs are downloaded and compared. Raises FetchError.
    """
    if not pin:
        # Without --pin every hash must be pinned: an empty one would accept any download.
        if not asset["sha256"]:
            raise FetchError(f"{asset['id']}: the archive is not pinned; run --pin --only {asset['id']}")
        unpinned = [e["path"] for e in asset["files"] + asset.get("extraFiles", []) if not e["sha256"]]
        if unpinned:
            raise FetchError(f"{asset['id']}: {', '.join(unpinned)} not pinned; run --pin --only {asset['id']}")
    archive = work / asset["archive"]
    actual = download(asset["url"], archive)
    _check_hash(asset["archive"], asset["sha256"], actual)
    if pin:
        published = github_digest(asset["url"])
        if published is not None and published != actual:
            raise FetchError(f"{asset['archive']}: SHA-256 {actual} differs from GitHub's published digest "
                             f"{published}")
        if published is not None:
            print(f"asset_fetch: {asset['archive']} matches GitHub's published digest", file=sys.stderr)
        asset["sha256"] = actual
    _, members = read_members(archive, asset)
    staged = []
    for entry in asset["files"]:
        data = members[entry["member"]]
        digest = hashlib.sha256(data).hexdigest()
        _check_hash(f"{asset['id']}: {entry['member']}", entry["sha256"], digest)
        second = cross_check_url(asset, entry)
        if pin and second:
            copy = work / ("crosscheck-" + entry["path"])
            other = download(second, copy)
            if other != digest:
                raise FetchError(f"{entry['path']}: the archive member and {second} differ")
            print(f"asset_fetch: {entry['path']} matches {second}", file=sys.stderr)
        if pin:
            entry["sha256"] = digest
        staged.append(Staged(entry["path"], data, digest))
    for extra in asset.get("extraFiles", []):
        target = work / ("extra-" + extra["path"])
        digest = download(extra["url"], target)
        _check_hash(f"{asset['id']}: {extra['url']}", extra["sha256"], digest)
        if pin:
            extra["sha256"] = digest
        staged.append(Staged(extra["path"], target.read_bytes(), digest))
    return staged


def write_staged(repo: Path, asset: dict, staged: list[Staged]) -> None:
    """
    Writes every staged file into <repo>/<dest>, each through a new uniquely named temporary file
    (mkstemp) and a rename, so only files this call created are ever removed. Raises FetchError
    or OSError.
    """
    dest = (repo / asset["dest"]).resolve()
    if repo.resolve() not in dest.parents:
        raise FetchError(f"{asset['dest']} is not inside the repository")
    dest.mkdir(parents=True, exist_ok=True)
    for item in staged:
        target = dest / item.path
        handle, name = tempfile.mkstemp(dir=dest, prefix=f".{item.path}.", suffix=".asset-fetch")
        temporary = Path(name)
        try:
            with os.fdopen(handle, "wb") as out:
                out.write(item.data)
                out.flush()
                os.fsync(out.fileno())
            os.replace(temporary, target)
        finally:
            if temporary.exists():
                temporary.unlink()


def format_manifest(value: object, indent: int = 0, inline: bool = False) -> str:
    """
    JSON text of the manifest: 2-space indentation, with each entry of a `files`,
    `extraFiles` or `copyright` list (and short string lists) kept on one line. Pure.
    """
    pad = " " * indent
    if inline or not isinstance(value, (dict, list)):
        return json.dumps(value, ensure_ascii=False)
    if isinstance(value, list):
        if not value:
            return "[]"
        if all(isinstance(v, str) for v in value):
            return json.dumps(value, ensure_ascii=False)
        items = [pad + "  " + format_manifest(v, indent + 2, isinstance(v, dict) and "path" in v) for v in value]
        return "[\n" + ",\n".join(items) + "\n" + pad + "]"
    items = [f'{pad}  {json.dumps(k)}: {format_manifest(v, indent + 2)}' for k, v in value.items()]
    return "{\n" + ",\n".join(items) + "\n" + pad + "}"


def write_manifest(path: Path, document: dict) -> None:
    """Rewrites the manifest (format_manifest, LF, trailing newline) through a temporary file."""
    temporary = path.with_name(path.name + f".tmp-{os.getpid()}")
    temporary.write_text(format_manifest(document) + "\n", encoding="utf-8", newline="\n")
    os.replace(temporary, path)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Fetch and verify FrostIDE's pinned bundled assets")
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent,
                        help="repository root (default: the checkout containing this script)")
    parser.add_argument("--manifest", type=Path, default=None, help=f"manifest (default <repo>/{MANIFEST_FILE})")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--verify", action="store_true", help="check the committed files (default; offline)")
    mode.add_argument("--fetch", action="store_true", help="download, verify and write the files")
    mode.add_argument("--pin", action="store_true", help="fill in empty hashes from a download")
    mode.add_argument("--list", action="store_true", help="print an archive's members")
    parser.add_argument("--only", action="append", default=None, help="asset id (repeatable)")
    parser.add_argument("--download-dir", type=Path, default=None,
                        help="downloads go to <dir>/<id>, which must be new or empty (default: a temp dir)")
    parser.add_argument("--allow-file-urls", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args(argv)

    repo = args.repo.resolve()
    manifest = args.manifest or repo / MANIFEST_FILE
    try:
        document, assets = load_manifest(manifest, args.allow_file_urls)
    except ManifestError as error:
        print(f"asset_fetch: {error}", file=sys.stderr)
        return 2
    known = {a["id"] for a in assets}
    for asset_id in args.only or []:
        if asset_id not in known:
            print(f"asset_fetch: unknown asset {asset_id!r} (known: {', '.join(sorted(known))})", file=sys.stderr)
            return 2
    selected = [a for a in assets if not args.only or a["id"] in args.only]
    if (args.pin or args.list) and (not args.only or len(args.only) != 1):
        print("asset_fetch: --pin and --list need exactly one --only <id>", file=sys.stderr)
        return 2

    if not (args.fetch or args.pin or args.list):
        problems = verify(repo, selected)
        for problem in problems:
            print(problem)
        print(f"asset_fetch: {len(selected)} asset(s), {len(problems)} problem(s)", file=sys.stderr)
        return 1 if problems else 0

    temporary = None
    base = args.download_dir
    if base is None:
        temporary = tempfile.mkdtemp(prefix="frost-assets-")
        base = Path(temporary)
    try:
        if args.list:
            asset = selected[0]
            work = fresh_dir(base, repo, asset["id"])
            archive = work / asset["archive"]
            actual = download(asset["url"], archive)
            if asset["sha256"]:
                _check_hash(asset["archive"], asset["sha256"], actual)
            else:
                print(f"asset_fetch: {asset['archive']} is unpinned; SHA-256 {actual}", file=sys.stderr)
            try:
                with zipfile.ZipFile(archive) as zf:
                    infos = zf.infolist()
            except _ARCHIVE_ERRORS as error:
                raise FetchError(f"{asset['archive']}: cannot read the archive: {error}") from error
            for info in infos:
                print(f"{info.file_size:>10} {info.filename}")
            return 0
        staged_all = []
        for asset in selected:
            work = fresh_dir(base, repo, asset["id"])
            staged_all.append((asset, stage_asset(asset, work, args.pin)))
        if args.pin:
            write_manifest(manifest, document)
            print(f"asset_fetch: pinned {selected[0]['id']} in {manifest}", file=sys.stderr)
            return 0
        # Every asset verified before any destination is touched.
        for asset, staged in staged_all:
            write_staged(repo, asset, staged)
            print(f"asset_fetch: {asset['id']}: wrote {len(staged)} file(s) to {asset['dest']}", file=sys.stderr)
        return 0
    except (FetchError, OSError) as error:
        print(f"asset_fetch: {error}", file=sys.stderr)
        return 1
    finally:
        if temporary is not None:
            shutil.rmtree(temporary, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
