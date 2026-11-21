#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Verifies the Windows stage's managed payloads and maps staged files to their component roles.
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import sys

import managed_server
import roslyn_host

sys.path.insert(0, str(Path(__file__).resolve().parent / "managed-debug"))
import install_payload  # noqa: E402


HOST = "roslyn"
DEBUG = "FrostManagedDebug"
DEBUG_MANIFEST = DEBUG + "/manifest.json"
DEBUG_NOTICES = DEBUG + "/adapter/notices/"
HOST_NOTICES = HOST + "/FrostIDE-Notices/"


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def assembly_version(stage: Path, folder: str, name: str) -> str:
    libraries = load_json(stage / folder / f"{name}.deps.json")["libraries"]
    prefix = name + "/"
    matches = [identity.removeprefix(prefix) for identity, library in libraries.items()
               if identity.startswith(prefix) and library["type"] == "project"]
    if len(matches) != 1 or not re.fullmatch(r"[0-9]+(?:\.[0-9]+){1,3}", matches[0]):
        raise ValueError(f"staged assembly version is ambiguous: {name}")
    return matches[0]


def verified_components(repo: Path, stage: Path, receipt: Path | None = None) -> tuple[list[dict], dict]:
    """Return explicit staged components after checking both complete manifest-bound subtrees."""
    for folder in (stage / HOST, stage / DEBUG):
        if not folder.is_dir() or folder.is_symlink():
            raise ValueError("both staged managed payloads must be ordinary directories")
        for entry in folder.rglob("*"):
            if entry.is_symlink() or not entry.resolve().is_relative_to(folder.resolve()):
                raise ValueError("staged managed payloads cannot contain links")
    admission = load_json(receipt) if receipt is not None else None
    if admission is not None and (admission.get("schema") != 1 or admission.get("kind") != "unsigned-release-packaging"
                                  or not re.fullmatch(r"[0-9a-f]{40}", admission["sourceCommit"])):
        raise ValueError("expected an unsigned Windows staging receipt with source identity")
    roslyn_host.verify_payload(stage / HOST, repo)
    manifest_hash = (admission["managedDebugManifestSha256"] if admission is not None
                     else managed_server.digest(stage / DEBUG_MANIFEST))
    managed_debug = install_payload.verify(stage / DEBUG, manifest_hash, rid="win-x64")
    managed_host = load_json(stage / HOST / roslyn_host.RECORD)
    roster = ({HOST + "/" + f for f in managed_host["files"]} |
              {DEBUG + "/" + f for f in managed_debug["files"]} |
              {HOST + "/" + roslyn_host.RECORD, DEBUG_MANIFEST, "LICENSE", "NOTICE"})
    host_files = {entry.relative_to(stage).as_posix() for entry in (stage / HOST).rglob("*") if entry.is_file()}
    if host_files != {name for name in roster if name.startswith(HOST + "/")}:
        raise ValueError("Roslyn stage contains missing or unlisted files")
    if admission is not None:
        expected = admission["files"]
        recorded = {name for name in expected if name.startswith((HOST + "/", DEBUG + "/"))}
        if recorded != roster - {"LICENSE", "NOTICE"}:
            raise ValueError("managed payload file sets differ from the Windows stage receipt")
        for name in sorted(roster):
            target = stage / name
            if target.is_symlink() or managed_server.digest(target) != expected[name]:
                raise ValueError(f"Windows stage receipt digest differs: {name}")
    if any(Path(f).name.casefold().startswith(("csharplanguageserver.", "ionide.", "fsharp", "argu."))
           for f in managed_host["files"]):
        raise ValueError("Windows stage contains a build-only csharp-ls host assembly")
    dependencies = load_json(repo / "sdk/managed-dependencies.json")
    expected_notices = dict(dependencies["notices"])
    expected_notices["managed-dependencies.json"] = managed_host["dependencyManifest"]
    actual_notices = {name.removeprefix("FrostIDE-Notices/"): sha for name, sha in managed_host["files"].items()
                      if name.startswith("FrostIDE-Notices/")}
    if actual_notices != expected_notices:
        raise ValueError("Roslyn staged notices differ from the audited dependency provenance")

    recipes = load_json(repo / "tools/managed-debug/inputs.json")
    netcore = recipes["netcoredbg"]
    if (managed_debug["netcoredbgCommit"] != netcore["commit"]
            or managed_debug["runtimeCommit"] != recipes["runtimeCommit"]):
        raise ValueError("managed-debug source commitments differ from the checked-in recipe")
    (shim,) = [p for p in recipes["packages"] if p["file"].startswith("microsoft.diagnostics.dbgshim.win-x64.")]
    shim_version = shim["file"].removeprefix("microsoft.diagnostics.dbgshim.win-x64.").removesuffix(".nupkg")
    shim_name = "Microsoft.Diagnostics.DbgShim.win-x64"
    common = ["LICENSE", "NOTICE"]
    for name in common:
        target = stage / name
        if not target.is_file() or target.is_symlink():
            raise ValueError(f"staged managed component notice is unavailable: {name}")
    provenance = {"debugManifestSha256": manifest_hash,
                  "observation": "retained-stage-receipt" if admission is not None else "observed-local-stage"}
    if admission is not None:
        provenance["stageSourceCommit"] = admission["sourceCommit"]
    recipe_names = {
        "cmake/toolchains/windows-clangcl.cmake", "sdk/sdk.lock.json",
        *("tools/managed-debug/" + name for name in (
            "audit_inputs.py", "build_payload.py", "build_pdb_guard.py", "dap-only.patch", "inputs.json",
            "offline_restore.py", "packages.lock.json", "prepare_source.py", "stage_scratch.py")),
    }
    recorded_recipes = {name.replace("\\", "/"): digest for name, digest in managed_debug["recipeSha256"].items()}
    if recorded_recipes.keys() != recipe_names:
        raise ValueError("managed-debug manifest recipe file list differs")
    differences = {}
    for path, recorded_hash in sorted(recorded_recipes.items()):
        content = (repo / path).read_bytes().replace(b"\r\n", b"\n")
        actual_hash = hashlib.sha256(content).hexdigest()
        if recorded_hash != actual_hash:
            differences[path] = {"stagedBuild": recorded_hash, "selectedCheckout": actual_hash}
    provenance["debugRecipeDifferences"] = differences
    components = [
        {
            "name": "FrostRoslynHost", "version": assembly_version(stage, HOST, "FrostRoslynHost"),
            "license": "Apache-2.0", "role": "FrostIDE-owned Roslyn language host built against audited offline assets",
            "source": "integrations/roslyn-host",
            "files": [HOST + "/" + name for name in (
                "FrostRoslynHost.exe", "FrostRoslynHost.dll", "FrostRoslynHost.deps.json",
                "FrostRoslynHost.runtimeconfig.json", "FrostRoslynHost.pdb", roslyn_host.RECORD)],
            "notices": [*common, *(HOST_NOTICES + f for f in sorted(expected_notices))],
        },
        {
            "name": "PdbGuard", "version": assembly_version(stage, DEBUG + "/PdbGuard", "PdbGuard"),
            "license": "Apache-2.0", "role": "FrostIDE-owned assembly, PDB, and source checksum verifier",
            "source": "integrations/managed-debug/PdbGuard",
            "files": [DEBUG + "/PdbGuard/" + name for name in (
                "PdbGuard.dll", "PdbGuard.deps.json", "PdbGuard.runtimeconfig.json")] + [DEBUG_MANIFEST],
            "notices": common,
        },
        {
            "name": "netcoredbg", "version": netcore["commit"], "license": netcore["license"],
            "role": "DAP-only netcoredbg adapter built from the audited source and local patch",
            "source": "tools/managed-debug/build_payload.py",
            "archive": f"https://codeload.github.com/Samsung/netcoredbg/tar.gz/{netcore['commit']}",
            "archiveSha256": netcore["sha256"],
            "files": [DEBUG + "/adapter/netcoredbg.exe", DEBUG + "/adapter/ManagedPart.dll",
                      DEBUG + "/adapter/scratch-manifest.json", DEBUG_MANIFEST],
            "notices": sorted(name for name in roster if name.startswith(DEBUG_NOTICES)),
        },
        {
            "name": shim_name, "version": shim_version, "license": shim["license"],
            "role": "Pinned Windows x64 CoreCLR debugging shim used by netcoredbg",
            "source": "tools/managed-debug/inputs.json",
            "archive": f"https://api.nuget.org/v3-flatcontainer/{shim_name.lower()}/{shim_version}/{shim['file']}",
            "archiveSha256": shim["sha256"], "purl": f"pkg:nuget/{shim_name}@{shim_version}",
            "files": [DEBUG + "/adapter/dbgshim.dll", DEBUG_MANIFEST],
            "notices": [DEBUG_NOTICES + "diagnostics-third-party.txt", DEBUG_NOTICES + "dotnet-MIT.txt",
                        DEBUG_NOTICES + "packages/" + shim["file"] + ".nuspec"],
        },
    ]
    if any(path not in roster for component in components for path in component["files"] + component["notices"]):
        raise ValueError("managed component inventory references a file outside the staged manifests")
    return components, provenance
