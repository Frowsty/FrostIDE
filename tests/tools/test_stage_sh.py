# SPDX-License-Identifier: Apache-2.0
#
# Tests for tools/stage.sh, the Linux stage (R0-close item 7), on a fixture build tree and SDK:
# frost, its crash handler, resources and licences, and the bundled clangd, lldb-dap and LuaLS in
# the layout the locators expect (luals-<version>/bin/lua-language-server); a missing tool fails
# before the destination is touched, and a failure after that leaves no partial folder. The fixture
# frost is a copy of /bin/true, a real dynamic executable, so the stage's ldd check runs for real.
# Linux only (registered by tests/tools/CMakeLists.txt when not WIN32); the real staged build is
# covered by test_staged_luals.py.

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

from tools_test_support import TOOLS, TempDirTestCase, clean_env

from stage_linux_qt import stage_qt

_LOCK = json.loads((TOOLS.parent / "sdk" / "sdk.lock.json").read_text(encoding="utf-8"))["linux"]
LLVM = f"llvm-{_LOCK['llvm']['version']}"
LLDB = f"lldb-{_LOCK['lldb']['version']}"
LUALS = f"luals-{_LOCK['luals']['version']}"


@unittest.skipUnless(sys.platform.startswith("linux"), "tools/stage.sh stages Linux builds")
class StageShTest(TempDirTestCase):
    def setUp(self) -> None:
        super().setUp()
        self.stage_repository = self.tmp / "repository"
        tools = self.stage_repository / "tools"
        tools.mkdir(parents=True)
        for name in ("stage.sh", "stage_linux_qt.py", "package_notices.py", "stage_luals.py", "install_desktop.py",
                     "licensecheck.py", "asset_fetch.py", "frost_lint.py"):
            shutil.copyfile(TOOLS / name, tools / name)
        for name in ("LICENSE", "NOTICE"):
            shutil.copyfile(TOOLS.parent / name, self.stage_repository / name)
        vendor = self.stage_repository / "third_party/tree-sitter"
        vendor.mkdir(parents=True)
        shutil.copyfile(TOOLS.parent / "third_party/tree-sitter/LICENSE", vendor / "LICENSE")
        (self.stage_repository / "integrations").symlink_to(TOOLS.parent / "integrations", target_is_directory=True)
        (self.stage_repository / "sdk").mkdir()

    def _executable(self, path: Path, content: bytes) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        path.chmod(0o755)

    def _fake_build(self, root: Path | None = None) -> Path:
        """A build tree with what the Debug frost target leaves in src/app/Debug."""
        build = (root or self.tmp) / "build"
        bin_dir = build / "src" / "app" / "Debug"
        bin_dir.mkdir(parents=True)
        shutil.copyfile("/bin/true", bin_dir / "frost")
        (bin_dir / "frost").chmod(0o755)
        # stage.sh inspects the snapshot validator's dynamic dependencies as well as frost's.
        shutil.copyfile("/bin/false", bin_dir / "frost-snapshot-validator")
        (bin_dir / "frost-snapshot-validator").chmod(0o755)
        for name in ("frost-analyzer", "frostide-mcp", "frost_held_read", "frost-extension-sandbox"):
            self._executable(bin_dir / name, b"fixture " + name.encode())
        self._executable(bin_dir / "frost-clang", b"isolated LLVM engine fixture")
        self._executable(bin_dir / "crashpad_handler", b"\x7fELF crashpad fixture")
        (bin_dir / "resources" / "themes").mkdir(parents=True)
        (bin_dir / "resources" / "themes" / "glacier-dark.ftheme").write_bytes(b"theme fixture")
        for name in ('lua54/lua', 'lua54/liblua54.so', 'licenses/Lua/lua.h'):
            path = bin_dir / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'Lua fixture')
        return build

    def _fake_sdk(self, root: Path | None = None) -> Path:
        """An SDK root holding every bundled tool, LuaLS with an install marker and a stale log."""
        sdk = (root or self.tmp) / "sdk"
        for name in (f"{LLVM}/bin/llvm-cov", f"{LLVM}/bin/llvm-profdata", f"{LLVM}/bin/clangd", f"{LLVM}/bin/clang-scan-deps", f"{LLDB}/bin/lldb-dap", f"{LLDB}/bin/lldb-argdumper",
                     f"{LUALS}/bin/lua-language-server"):
            self._executable(sdk / name, b"fixture " + name.encode())
        vendor_members = ("bin/lua-language-server", "LICENSE", "main.lua", "script/files.lua", "meta/template/math.lua")
        for name in (f"{LLVM}/lib/clang/23/include/stddef.h", f"{LLDB}/lib/liblldb.so.23.1.4",
                     *(f"{LUALS}/{name}" for name in vendor_members[1:]), f"{LUALS}/.frost-installed",
                     f"{LUALS}/log/service.log", f"{LUALS}/meta/Lua 5.4 en-us utf8/generated.lua"):
            (sdk / name).parent.mkdir(parents=True, exist_ok=True)
            (sdk / name).write_bytes(b"fixture " + name.encode())
        (sdk / LLDB / "lib" / "liblldb.so").symlink_to("liblldb.so.23.1.4")
        lock = json.loads((TOOLS.parent / "sdk/sdk.lock.json").read_text(encoding="utf-8"))
        pin = lock["linux"]["luals"]
        archive = sdk / "_downloads" / pin["file"]
        archive.parent.mkdir()
        with tarfile.open(archive, "w:gz") as bundle:
            for name in vendor_members:
                bundle.add(sdk / LUALS / name, arcname=name)
        pin["sha256"] = hashlib.sha256(archive.read_bytes()).hexdigest()
        (self.stage_repository / "sdk/sdk.lock.json").write_text(json.dumps(lock), encoding="utf-8")
        (sdk / LUALS / "script/files.lua").write_bytes(b"modified installed script")
        qt_version = lock["qt"]["version"]
        for name in (f'lib/libQt6Core.so.{qt_version}', 'plugins/platforms/libqxcb.so', 'plugins/platforms/libqoffscreen.so', 'plugins/tls/libqopensslbackend.so'):
            path = sdk / 'qt' / qt_version / 'gcc_64' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile('/bin/true', path)
        qt_lib = sdk / 'qt' / qt_version / 'gcc_64/lib'
        (qt_lib / 'libQt6Core.so.6').symlink_to(f'libQt6Core.so.{qt_version}')
        (qt_lib / 'libQt6Core.so').symlink_to('libQt6Core.so.6')
        return sdk

    def _stage(self, build: Path, sdk: Path, destination: Path, *extra: str) -> subprocess.CompletedProcess:
        env = clean_env()
        env.pop("FROST_SDK", None)
        return subprocess.run(["bash", str(self.stage_repository / "tools/stage.sh"), "--dest", str(destination), "--config", "Debug",
                               "--build-dir", str(build), "--sdk-root", str(sdk), "--skip-build", *extra],
                              capture_output=True, text=True, env=env, check=False)

    def test_qt_library_aliases_share_a_relocatable_payload(self) -> None:
        sdk = self._fake_sdk()
        destination = self.tmp / 'stage'
        destination.mkdir()
        shutil.copyfile('/bin/true', destination / 'frost')
        stage_qt(self.stage_repository, sdk, destination, debug=True)
        lib = destination / 'qt/lib'
        lock = json.loads((self.stage_repository / 'sdk/sdk.lock.json').read_text(encoding='utf-8'))
        payload = Path(f'libQt6Core.so.{lock["qt"]["version"]}')
        for alias in ('libQt6Core.so', 'libQt6Core.so.6'):
            self.assertEqual((lib / alias).readlink(), payload)
            self.assertEqual((lib / alias).read_bytes(), (lib / payload).read_bytes())
        qt = sdk / 'qt' / lock['qt']['version'] / 'gcc_64'
        unshipped = qt / 'other.so'
        unshipped.write_bytes(b'not a staged library')
        alias = qt / 'lib/libQt6Core.so'
        alias.unlink()
        alias.symlink_to('../other.so')
        with self.assertRaisesRegex(ValueError, 'does not resolve to a shipped library'):
            stage_qt(self.stage_repository, sdk, destination, debug=True, check_only=True)

    def test_frost_and_the_bundled_tools_are_staged_in_the_locator_layout(self) -> None:
        build, sdk = self._fake_build(), self._fake_sdk()
        destination = self.tmp / "out" / "preview"
        result = self._stage(build, sdk, destination)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"stage: ok {destination}", result.stdout)
        self.assertEqual((destination / "install_desktop.py").read_bytes(), (TOOLS / "install_desktop.py").read_bytes())
        for name in ("resources/themes/glacier-dark.ftheme", "LICENSE", "NOTICE", "install_desktop.py",
                     "licenses/luals-LICENSE.txt", "licenses/tree-sitter-LICENSE",
                     "lua54/lua", "lua54/liblua54.so", "licenses/Lua/lua.h",
                     "qt/lib/libQt6Core.so.6", "qt/plugins/platforms/libqxcb.so",
                     "qt/plugins/platforms/libqoffscreen.so", "qt/plugins/tls/libqopensslbackend.so",
                     "unity/com.frostide.editor/package.json",
                     f"{LLVM}/bin/llvm-cov", f"{LLVM}/bin/llvm-profdata", f"{LLVM}/bin/clangd", f"{LLVM}/bin/clang-scan-deps", f"{LLVM}/lib/clang/23/include/stddef.h", f"{LLDB}/bin/lldb-dap",
                     f"{LLDB}/bin/lldb-argdumper", f"{LLDB}/lib/liblldb.so.23.1.4", f"{LUALS}/bin/lua-language-server",
                     f"{LUALS}/main.lua", f"{LUALS}/script/files.lua"):
            self.assertTrue((destination / name).is_file(), name)
        bin_dir = build / "src" / "app" / "Debug"
        for name in ("frost-clang", "frost-analyzer", "frostide-mcp", "frost-snapshot-validator",
                     "frost_held_read", "crashpad_handler", "frost-extension-sandbox"):
            self.assertTrue(os.access(destination / name, os.X_OK), name)
            self.assertEqual((destination / name).read_bytes(), (bin_dir / name).read_bytes(), name)
        self.assertTrue(os.access(destination / LUALS / "bin" / "lua-language-server", os.X_OK))
        self.assertEqual((destination / "frost.bin").read_bytes(), (bin_dir / "frost").read_bytes())
        launcher = subprocess.run([destination / "frost"], capture_output=True, text=True, check=False)
        self.assertEqual(launcher.returncode, 0, launcher.stdout + launcher.stderr)
        self.assertTrue((destination / LLDB / "lib" / "liblldb.so").is_symlink(), "the soname link is kept")
        self.assertFalse((destination / LUALS / ".frost-installed").exists(), "the SDK's install marker")
        self.assertFalse((destination / LUALS / "log").exists(), "a log a run left in the SDK")
        self.assertFalse((destination / LUALS / "meta/Lua 5.4 en-us utf8").exists(), "SDK-generated meta")
        self.assertEqual((destination / LUALS / "meta/template/math.lua").read_bytes(),
                         (sdk / LUALS / "meta/template/math.lua").read_bytes())
        self.assertEqual((destination / LUALS / "script/files.lua").read_bytes(),
                         f"fixture {LUALS}/script/files.lua".encode())
        self.assertEqual((destination / "licenses/luals-LICENSE.txt").read_bytes(),
                         (destination / LUALS / "LICENSE").read_bytes())

    def test_unverified_icu_notice_fails_before_staging(self) -> None:
        build, sdk = self._fake_build(), self._fake_sdk()
        notice = self.tmp / "incorrect ICU notice.txt"
        notice.write_bytes(b"incorrect notice")
        destination = self.tmp / "out"

        result = self._stage(build, sdk, destination, "--icu-notice", str(notice))

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ICU notice does not match sdk.lock", result.stderr)
        self.assertFalse(destination.exists())

    def test_pinned_icu_notice_is_copied_byte_for_byte_without_writing_to_sdk(self) -> None:
        sdk = self._fake_sdk()
        repository = self.tmp / "repo"
        lock = repository / "sdk/sdk.lock.json"
        lock.parent.mkdir(parents=True)
        fixture_pin = json.loads((TOOLS.parent / "sdk/sdk.lock.json").read_text(encoding="utf-8"))
        notice = self.tmp / "pinned ICU notice.txt"
        notice_bytes = b"Unicode ICU fixture\r\n"
        notice.write_bytes(notice_bytes)
        fixture_pin["qt"]["linux"]["icuNotice"]["sha256"] = hashlib.sha256(notice_bytes).hexdigest()
        lock.write_text(json.dumps(fixture_pin), encoding="utf-8")
        qt = sdk / "qt" / fixture_pin["qt"]["version"] / "gcc_64"
        (qt / "lib/libicuuc.so.73").write_bytes(b"ICU fixture")
        destination = self.tmp / "stage"
        with self.assertRaisesRegex(ValueError, "bundled ICU requires the pinned --icu-notice"):
            stage_qt(repository, sdk, destination, debug=True, check_only=True)
        self.assertFalse(destination.exists())
        destination.mkdir()
        self._executable(destination / "frost", b"FrostIDE executable fixture")

        stage_qt(repository, sdk, destination, debug=True, icu_notice=notice)

        self.assertEqual((destination / "licenses/ICU.txt").read_bytes(), notice_bytes)
        self.assertFalse((qt / "licenses").exists())
        self.assertEqual(notice.read_bytes(), notice_bytes)

        cached = sdk / "_downloads" / fixture_pin["qt"]["linux"]["icuNotice"]["file"]
        cached.parent.mkdir(exist_ok=True)
        cached.write_bytes(b"incorrect cached notice")
        cached_stage = self.tmp / "cached-stage"
        with self.assertRaisesRegex(ValueError, "ICU notice does not match sdk.lock"):
            stage_qt(repository, sdk, cached_stage, debug=True, check_only=True)
        self.assertFalse(cached_stage.exists())
        cached.write_bytes(notice_bytes)
        cached_stage.mkdir()
        self._executable(cached_stage / "frost", b"FrostIDE executable fixture")
        stage_qt(repository, sdk, cached_stage, debug=True)
        self.assertEqual((cached_stage / "licenses/ICU.txt").read_bytes(), notice_bytes)

    def test_a_missing_bundled_tool_fails_before_staging(self) -> None:
        cases = {"clang-scan-deps": f"{LLVM}/bin/clang-scan-deps", "LLVM coverage tool": f"{LLVM}/bin/llvm-cov", "clangd": f"{LLVM}/bin/clangd", "clangd resource directory": f"{LLVM}/lib/clang",
                 "lldb-dap file": f"{LLDB}/bin/lldb-dap"}
        for tool, missing in cases.items():
            with self.subTest(tool=tool), tempfile.TemporaryDirectory(dir=self.tmp) as scratch:
                root = Path(scratch)
                build, sdk = self._fake_build(root), self._fake_sdk(root)
                target = sdk / missing
                shutil.rmtree(target) if target.is_dir() else target.unlink()
                destination = root / "missing-tool"
                result = self._stage(build, sdk, destination)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(f"stage: FAIL missing bundled {tool}", result.stdout)
                self.assertFalse(destination.exists(), "nothing is staged without the tool")

    def test_missing_or_changed_luals_archive_fails_before_staging(self) -> None:
        build, sdk = self._fake_build(), self._fake_sdk()
        archive = sdk / "_downloads" / _LOCK["luals"]["file"]
        archive.write_bytes(archive.read_bytes() + b"changed archive")
        destination = self.tmp / "changed-luals"
        result = self._stage(build, sdk, destination)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("archive SHA-256 does not match", result.stderr)
        self.assertFalse(destination.exists())

        archive.unlink()
        result = self._stage(build, sdk, destination)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("missing bundled LuaLS archive", result.stderr)
        self.assertFalse(destination.exists())

    def test_no_sdk_root_is_guessed(self) -> None:
        build = self._fake_build()
        env = clean_env()
        env.pop("FROST_SDK", None)
        result = subprocess.run(["bash", str(TOOLS / "stage.sh"), "--dest", str(self.tmp / "x"), "--config", "Debug",
                                 "--build-dir", str(build), "--skip-build"],
                                capture_output=True, text=True, env=env, check=False)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("no SDK root", result.stdout)
        self.assertFalse((self.tmp / "x").exists())

    def test_a_late_failure_removes_the_partial_stage(self) -> None:
        # A clangd resource is unreadable, so the copy fails part-way through step 5, after
        # FrostIDE's own files are already in the destination.
        build, sdk = self._fake_build(), self._fake_sdk()
        resource = sdk / LLVM / "lib/clang/23/include/stddef.h"
        resource.chmod(0o000)
        destination = self.tmp / "late"
        try:
            result = self._stage(build, sdk, destination)
        finally:
            resource.chmod(0o644)
        if os.geteuid() == 0:
            self.skipTest("root reads unreadable files; the copy cannot fail this way")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("cannot copy the clangd resource directory", result.stdout)
        self.assertIn(f"removed the partial {destination}", result.stdout)
        self.assertFalse(destination.exists(), "no partial stage is left behind")

    def test_a_late_failure_empties_an_existing_empty_destination(self) -> None:
        build, sdk = self._fake_build(), self._fake_sdk()
        resource = sdk / LLVM / "lib/clang/23/include/stddef.h"
        resource.chmod(0o000)
        destination = self.tmp / "given"
        destination.mkdir()
        try:
            result = self._stage(build, sdk, destination)
        finally:
            resource.chmod(0o644)
        if os.geteuid() == 0:
            self.skipTest("root reads unreadable files; the copy cannot fail this way")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("cannot copy the clangd resource directory", result.stdout)
        self.assertTrue(destination.is_dir(), "a folder the caller made stays")
        self.assertEqual(list(destination.iterdir()), [], "but holds nothing of the failed stage")

    def test_an_existing_stage_needs_force(self) -> None:
        build, sdk = self._fake_build(), self._fake_sdk()
        destination = self.tmp / "again"
        self.assertEqual(self._stage(build, sdk, destination).returncode, 0)
        refused = self._stage(build, sdk, destination)
        self.assertEqual(refused.returncode, 1)
        self.assertIn("is not empty (use --force", refused.stdout)
        self.assertTrue((destination / "frost").is_file(), "a refused stage keeps the old one")
        self.assertEqual(self._stage(build, sdk, destination, "--force").returncode, 0)

    def test_unresolved_libraries_fail_the_stage(self) -> None:
        build, sdk = self._fake_build(), self._fake_sdk()
        # A copy of the dynamic loader's own test: an ELF whose NEEDED library does not exist.
        compiler = shutil.which("clang") or shutil.which("cc")
        if compiler is None:
            self.fail("clang of the pinned LLVM is required (source ~/sdk/env.sh)")
        library_dir = self.tmp / "lib"
        library_dir.mkdir()
        (self.tmp / "gone.c").write_text("int gone(void) { return 0; }\n", encoding="utf-8")
        (self.tmp / "main.c").write_text("int gone(void);\nint main(void) { return gone(); }\n", encoding="utf-8")
        subprocess.run([compiler, "-shared", "-fPIC", "-o", str(library_dir / "libgone.so"), str(self.tmp / "gone.c")],
                       check=True)
        frost = build / "src" / "app" / "Debug" / "frost"
        subprocess.run([compiler, "-o", str(frost), str(self.tmp / "main.c"), f"-L{library_dir}", "-lgone"], check=True)
        (library_dir / "libgone.so").unlink()
        destination = self.tmp / "unresolved"
        result = self._stage(build, sdk, destination)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("libraries that do not resolve: libgone.so => not found", result.stdout)
        self.assertFalse(destination.exists())


if __name__ == "__main__":
    unittest.main()
