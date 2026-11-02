# SPDX-License-Identifier: Apache-2.0
# Pin, dependency-graph and archive extraction tests for the managed server installer.
from __future__ import annotations
import hashlib
import json
from pathlib import Path
import unittest
import zipfile
from tools_test_support import TempDirTestCase
import managed_server


def write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


class ManagedServerTest(TempDirTestCase):
    def setup_package(self, members: dict[str, bytes] | None = None) -> Path:
        repo = self.tmp
        write(repo / "NOTICE", b"test notices")
        write(repo / "third_party/managed-notices/original.txt", b"original notice")
        manifest = {"server": "csharp-ls/0.28.0", "packages": [{"name": "A", "version": "1"}],
                    "notices": {"original.txt": hashlib.sha256(b"original notice").hexdigest()},
                    "graphs": {"CSharpLanguageServer.deps.json": {"packages": ["A/1"], "projects": ["server/1"]}},
                    "hostAssets": {}}
        manifest_bytes = json.dumps(manifest).encode()
        write(repo / "sdk/managed-dependencies.json", manifest_bytes)
        archive = repo / "server.nupkg"
        with zipfile.ZipFile(archive, "w") as package:
            package.writestr("tools/net10.0/any/CSharpLanguageServer.dll", b"server")
            package.writestr("tools/net10.0/any/CSharpLanguageServer.deps.json", json.dumps(
                {"libraries": {"A/1": {"type": "package"}, "server/1": {"type": "project"}}}))
            for name, content in (members or {}).items():
                package.writestr("tools/net10.0/any/" + name, content)
        pin = {"managedServer": {"name": "csharp-ls", "version": "0.28.0",
                "sha256": managed_server.digest(archive), "manifestSha256": hashlib.sha256(manifest_bytes).hexdigest(),
                "entry": "tools/net10.0/any/CSharpLanguageServer.dll"}}
        write(repo / "sdk/sdk.lock.json", json.dumps(pin).encode())
        return archive

    def test_verified_package_stages_original_notices(self) -> None:
        archive = self.setup_package()
        destination = self.tmp / "install"
        managed_server.install(archive, destination, self.tmp)
        self.assertEqual((destination / "CSharpLanguageServer.dll").read_bytes(), b"server")
        self.assertEqual((destination / "FrostIDE-Notices/original.txt").read_bytes(), b"original notice")
        managed_server.verify_installed(archive, destination, self.tmp)
        (destination / "CSharpLanguageServer.dll").write_bytes(b"modified")
        with self.assertRaisesRegex(ValueError, "file changed"):
            managed_server.verify_installed(archive, destination, self.tmp)
        (destination / "CSharpLanguageServer.dll").write_bytes(b"server")
        with self.assertRaises(ValueError):
            managed_server.install(archive, destination, self.tmp)
        self.assertEqual((destination / "CSharpLanguageServer.dll").read_bytes(), b"server")

    def test_modified_archive_notice_and_manifest_are_rejected(self) -> None:
        archive = self.setup_package()
        archive.write_bytes(archive.read_bytes() + b"tampered")
        with self.assertRaisesRegex(ValueError, "archive SHA-256"):
            managed_server.verify(archive, self.tmp)
        archive = self.setup_package()
        write(self.tmp / "third_party/managed-notices/original.txt", b"changed")
        with self.assertRaisesRegex(ValueError, "notice hash"):
            managed_server.verify(archive, self.tmp)
        archive = self.setup_package()
        write(self.tmp / "sdk/managed-dependencies.json", b"{}")
        with self.assertRaisesRegex(ValueError, "manifest SHA-256"):
            managed_server.verify(archive, self.tmp)

    def test_audit_checks_host_asset_against_original_nuget_package(self) -> None:
        cache = self.tmp / "cache"
        cache.mkdir()
        archive = cache / "a.1.nupkg"
        nuspec = b"<package><metadata><id>A</id><version>1</version><authors>Author</authors><copyright>Copyright</copyright><license>MIT</license></metadata></package>"
        with zipfile.ZipFile(archive, "w") as package:
            package.writestr("a.nuspec", nuspec)
            package.writestr("lib/net/a.dll", b"verified host")
        metadata = {"packages": [{"name": "A", "version": "1", "archiveSha256": managed_server.digest(archive),
                                  "nuspecSha256": hashlib.sha256(nuspec).hexdigest(), "license": "MIT",
                                  "authors": "Author", "copyright": "Copyright", "notices": []}],
                    "notices": {}, "hostAssets": {"BuildHost-net472/a.dll": {
                        "sha256": hashlib.sha256(b"verified host").hexdigest(), "sourcePackage": "A/1",
                        "sourceMember": "lib/net/a.dll"}}}
        managed_server.audit_packages(cache, metadata)
        metadata["hostAssets"]["BuildHost-net472/a.dll"]["sha256"] = hashlib.sha256(b"other").hexdigest()
        with self.assertRaisesRegex(ValueError, "host asset provenance differs"):
            managed_server.audit_packages(cache, metadata)

    def test_nested_only_dependency_omission_is_rejected(self) -> None:
        archive = self.setup_package()
        nested_path = "BuildHost-netcore/host.deps.json"
        nested_bytes = json.dumps({"libraries": {"B/2": {"type": "package"}}}).encode()
        with zipfile.ZipFile(archive, "a") as package:
            package.writestr("tools/net10.0/any/" + nested_path, nested_bytes)
        manifest_path = self.tmp / "sdk/managed-dependencies.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["graphs"][nested_path] = {"packages": ["B/2"], "projects": []}
        manifest["hostAssets"][nested_path] = {"sha256": hashlib.sha256(nested_bytes).hexdigest(),
                                                "sourcePackage": "A/1", "sourceMember": "test"}
        # Deliberately omit B/2 from the licence package list.
        data = json.dumps(manifest).encode()
        write(manifest_path, data)
        lock_path = self.tmp / "sdk/sdk.lock.json"
        lock = json.loads(lock_path.read_text())
        lock["managedServer"]["sha256"] = managed_server.digest(archive)
        lock["managedServer"]["manifestSha256"] = hashlib.sha256(data).hexdigest()
        write(lock_path, json.dumps(lock).encode())
        with self.assertRaisesRegex(ValueError, "dependency graph differs"):
            managed_server.verify(archive, self.tmp)

    def test_rooted_paths_are_rejected_in_install_and_verification(self) -> None:
        archive = self.setup_package({"/absolute/escape": b"unsafe"})
        destination = self.tmp / "install"
        with self.assertRaisesRegex(ValueError, "unsafe archive path"):
            managed_server.install(archive, destination, self.tmp)
        destination.mkdir()
        with self.assertRaisesRegex(ValueError, "unsafe archive path"):
            managed_server.verify_installed(archive, destination, self.tmp)

    def test_graph_mismatch_and_path_traversal_are_rejected(self) -> None:
        archive = self.setup_package({"../../escape": b"unsafe"})
        with self.assertRaisesRegex(ValueError, "unsafe archive path"):
            managed_server.install(archive, self.tmp / "install", self.tmp)
        self.assertFalse((self.tmp / "escape").exists())
        manifest_path = self.tmp / "sdk/managed-dependencies.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["packages"] = [{"name": "Other", "version": "1"}]
        data = json.dumps(manifest).encode()
        write(manifest_path, data)
        lock_path = self.tmp / "sdk/sdk.lock.json"
        lock = json.loads(lock_path.read_text())
        lock["managedServer"]["manifestSha256"] = hashlib.sha256(data).hexdigest()
        write(lock_path, json.dumps(lock).encode())
        with self.assertRaisesRegex(ValueError, "dependency graph differs"):
            managed_server.verify(archive, self.tmp)


if __name__ == "__main__":
    unittest.main()
