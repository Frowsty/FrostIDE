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

import json
import os
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

from tools_test_support import TOOLS, TempDirTestCase, clean_env

_LOCK = json.loads((TOOLS.parent / "sdk" / "sdk.lock.json").read_text(encoding="utf-8"))["linux"]
LLVM = f"llvm-{_LOCK['llvm']['version']}"
LLDB = f"lldb-{_LOCK['lldb']['version']}"
LUALS = f"luals-{_LOCK['luals']['version']}"


@unittest.skipUnless(sys.platform.startswith("linux"), "tools/stage.sh stages Linux builds")
class StageShTest(TempDirTestCase):
    def _executable(self, path: Path, content: bytes) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        path.chmod(0o755)

    def _fake_build(self) -> Path:
        """A build tree with what the Debug frost target leaves in src/app/Debug."""
        build = self.tmp / "build"
        bin_dir = build / "src" / "app" / "Debug"
        bin_dir.mkdir(parents=True)
        shutil.copyfile("/bin/true", bin_dir / "frost")
        (bin_dir / "frost").chmod(0o755)
        self._executable(bin_dir / "crashpad_handler", b"\x7fELF crashpad fixture")
        (bin_dir / "resources" / "themes").mkdir(parents=True)
        (bin_dir / "resources" / "themes" / "glacier-dark.ftheme").write_bytes(b"theme fixture")
        return build

    def _fake_sdk(self) -> Path:
        """An SDK root holding every bundled tool, LuaLS with an install marker and a stale log."""
        sdk = self.tmp / "sdk"
        for name in (f"{LLVM}/bin/clangd", f"{LLDB}/bin/lldb-dap", f"{LLDB}/bin/lldb-argdumper",
                     f"{LUALS}/bin/lua-language-server"):
            self._executable(sdk / name, b"fixture " + name.encode())
        for name in (f"{LLVM}/lib/clang/23/include/stddef.h", f"{LLDB}/lib/liblldb.so.23.1.4", f"{LUALS}/main.lua",
                     f"{LUALS}/LICENSE", f"{LUALS}/script/files.lua", f"{LUALS}/.frost-installed",
                     f"{LUALS}/log/service.log"):
            (sdk / name).parent.mkdir(parents=True, exist_ok=True)
            (sdk / name).write_bytes(b"fixture " + name.encode())
        (sdk / LLDB / "lib" / "liblldb.so").symlink_to("liblldb.so.23.1.4")
        return sdk

    def _stage(self, build: Path, sdk: Path, destination: Path, *extra: str) -> subprocess.CompletedProcess:
        env = clean_env()
        env.pop("FROST_SDK", None)
        return subprocess.run(["bash", str(TOOLS / "stage.sh"), "--dest", str(destination), "--config", "Debug",
                               "--build-dir", str(build), "--sdk-root", str(sdk), "--skip-build", *extra],
                              capture_output=True, text=True, env=env, check=False)

    def test_frost_and_the_bundled_tools_are_staged_in_the_locator_layout(self) -> None:
        build, sdk = self._fake_build(), self._fake_sdk()
        destination = self.tmp / "out" / "preview"
        result = self._stage(build, sdk, destination)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"stage: ok {destination}", result.stdout)
        for name in ("frost", "crashpad_handler", "resources/themes/glacier-dark.ftheme", "LICENSE", "NOTICE",
                     "licenses/luals-LICENSE.txt", "licenses/tree-sitter-LICENSE",
                     "unity/com.frostide.editor/package.json",
                     f"{LLVM}/bin/clangd", f"{LLVM}/lib/clang/23/include/stddef.h", f"{LLDB}/bin/lldb-dap",
                     f"{LLDB}/bin/lldb-argdumper", f"{LLDB}/lib/liblldb.so.23.1.4", f"{LUALS}/bin/lua-language-server",
                     f"{LUALS}/main.lua", f"{LUALS}/script/files.lua"):
            self.assertTrue((destination / name).is_file(), name)
        self.assertTrue(os.access(destination / LUALS / "bin" / "lua-language-server", os.X_OK))
        self.assertEqual((destination / "frost").read_bytes(), Path("/bin/true").read_bytes())
        self.assertTrue((destination / LLDB / "lib" / "liblldb.so").is_symlink(), "the soname link is kept")
        self.assertFalse((destination / LUALS / ".frost-installed").exists(), "the SDK's install marker")
        self.assertFalse((destination / LUALS / "log").exists(), "a log a run left in the SDK")

    def test_a_missing_bundled_tool_fails_before_staging(self) -> None:
        cases = {"clangd": f"{LLVM}/bin/clangd", "clangd resource directory": f"{LLVM}/lib/clang",
                 "lldb-dap file": f"{LLDB}/bin/lldb-dap", "LuaLS file": f"{LUALS}/bin/lua-language-server"}
        for tool, missing in cases.items():
            with self.subTest(tool=tool):
                build, sdk = self._fake_build(), self._fake_sdk()
                target = sdk / missing
                shutil.rmtree(target) if target.is_dir() else target.unlink()
                destination = self.tmp / "missing-tool"
                result = self._stage(build, sdk, destination)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(f"stage: FAIL missing bundled {tool}", result.stdout)
                self.assertFalse(destination.exists(), "nothing is staged without the tool")
                shutil.rmtree(self.tmp / "build")
                shutil.rmtree(self.tmp / "sdk")

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
        # One of LuaLS's scripts is unreadable, so the copy fails part-way through step 5, after
        # FrostIDE's own files are already in the destination.
        build, sdk = self._fake_build(), self._fake_sdk()
        (sdk / LUALS / "script" / "files.lua").chmod(0o000)
        destination = self.tmp / "late"
        try:
            result = self._stage(build, sdk, destination)
        finally:
            (sdk / LUALS / "script" / "files.lua").chmod(0o644)
        if os.geteuid() == 0:
            self.skipTest("root reads unreadable files; the copy cannot fail this way")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("cannot copy LuaLS", result.stdout)
        self.assertIn(f"removed the partial {destination}", result.stdout)
        self.assertFalse(destination.exists(), "no partial stage is left behind")

    def test_a_late_failure_empties_an_existing_empty_destination(self) -> None:
        build, sdk = self._fake_build(), self._fake_sdk()
        (sdk / LUALS / "script" / "files.lua").chmod(0o000)
        destination = self.tmp / "given"
        destination.mkdir()
        try:
            result = self._stage(build, sdk, destination)
        finally:
            (sdk / LUALS / "script" / "files.lua").chmod(0o644)
        if os.geteuid() == 0:
            self.skipTest("root reads unreadable files; the copy cannot fail this way")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
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
