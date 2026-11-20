# SPDX-License-Identifier: Apache-2.0
# Exercises staged desktop registration and removal inside a disposable XDG profile.
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import unittest
from unittest.mock import patch

from tools_test_support import TOOLS, TempDirTestCase, clean_env

DESKTOP = "org.frostide.FrostIDE.desktop"


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux desktop registration")
class LinuxDesktopTest(TempDirTestCase):
    def setUp(self):
        super().setUp()
        self.data = self.tmp / "data"
        self.config = self.tmp / "config"
        self.home = self.tmp / "home"
        self.application = self.data / "applications" / DESKTOP
        self.mime = self.data / "mime" / "packages" / "org.frostide.FrostIDE.xml"
        self.receipt = self.data / "frostide" / "shell-registration.json"
        self.capture = self.tmp / "captured.json"
        for directory in (self.config, self.home, self.application.parent):
            directory.mkdir(parents=True)
        self.env = clean_env(HOME=str(self.home), XDG_DATA_HOME=str(self.data),
                             XDG_CONFIG_HOME=str(self.config), XDG_CACHE_HOME=str(self.tmp / "cache"),
                             XDG_DATA_DIRS="/usr/share", XDG_CONFIG_DIRS=str(self.tmp / "system-config"),
                             XDG_CURRENT_DESKTOP="GNOME", FROST_DESKTOP_CAPTURE=str(self.capture))
        for variable in ("DE", "DBUS_SESSION_BUS_ADDRESS", "DESKTOP_SESSION", "XDG_RUNTIME_DIR"):
            self.env.pop(variable, None)
        self.defaults = self.config / "mimeapps.list"
        self.defaults.write_text("[Default Applications]\ntext/x-lua=other.desktop;\n", encoding="utf-8")
        (self.application.parent / "other.desktop").write_text(
            "[Desktop Entry]\nType=Application\nName=Other\nExec=/bin/true %f\nMimeType=text/x-lua;\n",
            encoding="utf-8",
        )
        self.package = self._package('Frost = %f [] $ ` " \\ stage')

    def _package(self, name):
        package = self.tmp / name
        package.mkdir()
        shutil.copyfile(TOOLS / "install_desktop.py", package / "install_desktop.py")
        shutil.copyfile("/bin/true", package / "frost.bin")
        launcher = package / "frost"
        launcher.write_text(
            '#!/usr/bin/env bash\nexec python3 -c \'import json, os, sys; '
            'json.dump(sys.argv[1:], open(os.environ["FROST_DESKTOP_CAPTURE"], "w"))\' "$@"\n',
            encoding="utf-8",
        )
        launcher.chmod(0o755)
        return package

    def _run(self, *arguments):
        return subprocess.run(arguments, capture_output=True, text=True, env=self.env, check=False, timeout=20)

    def _install(self, action, package=None, expected=0):
        result = self._run(sys.executable, "-B", (package or self.package) / "install_desktop.py", action)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result

    def test_register_launch_uninstall_preserves_current_default(self):
        original_defaults = self.defaults.read_bytes()
        self._install("register")
        self.assertTrue(self.receipt.is_file())
        self.assertIn("text/x-cmake;text/x-lua;", self.application.read_text(encoding="utf-8"))
        self._install("register")  # Same package and unchanged receipt are idempotent.
        self.assertEqual(self.defaults.read_bytes(), original_defaults)
        self.assertTrue(self.mime.is_file())

        project = self.tmp / 'project 100%f $ ` " name.frostproj'
        project.write_text('{"name": "Example"}\n', encoding="utf-8")
        if validator := shutil.which("desktop-file-validate"):
            result = self._run(validator, self.application)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        gio = shutil.which("gio")
        if (xdg_mime := shutil.which("xdg-mime")) and gio:
            for name, kind in ((project.name, "application/x-frostide-project"),
                               ("Sample.sln", "text/x-ms-sln"), ("Sample.slnx", "application/x-ms-slnx")):
                path = self.tmp / name
                if not path.exists():
                    path.write_text("fixture\n", encoding="utf-8")
                result = self._run(xdg_mime, "query", "filetype", path)
                self.assertEqual(result.stdout.strip(), kind, result.stdout + result.stderr)
            result = self._run(xdg_mime, "query", "default", "text/x-lua")
            self.assertEqual(result.stdout.strip(), "other.desktop", result.stdout + result.stderr)
        if gio:
            for kind in ("application/x-frostide-project", "text/x-ms-sln", "application/x-ms-slnx",
                         "text/x-cmake", "text/x-lua"):
                result = self._run(gio, "mime", kind)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(DESKTOP, result.stdout)
            result = self._run(gio, "launch", self.application, project)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                try:
                    captured = json.loads(self.capture.read_text(encoding="utf-8"))
                    break
                except (FileNotFoundError, json.JSONDecodeError):
                    time.sleep(0.05)
            else:
                self.fail("the package launcher did not receive the test file")
            self.assertEqual(captured, ["--", str(project)])

        self._install("unregister")
        self._install("unregister")
        self.assertFalse(self.application.exists())
        self.assertFalse(self.mime.exists())
        self.assertFalse(self.receipt.exists())
        self.assertEqual(self.defaults.read_bytes(), original_defaults)
        self.assertTrue((self.application.parent / "other.desktop").is_file())
        if gio:
            self.assertNotIn(DESKTOP, self._run(gio, "mime", "text/x-lua").stdout)

    def test_missing_cache_updater_does_not_publish_registration(self):
        tools = self.tmp / "missing-tool-path"
        tools.mkdir()
        (tools / "update-mime-database").symlink_to("/bin/false")
        with patch.dict(self.env, PATH=str(tools)):
            result = self._install("register", expected=1)
        self.assertIn("update-desktop-database is required for per-user registration", result.stderr)
        self.assertFalse(self.application.exists())
        self.assertFalse(self.mime.parent.exists())
        self.assertFalse(self.receipt.parent.exists())
        self.assertFalse((self.application.parent / "mimeinfo.cache").exists())

    def test_failed_cache_update_rolls_back_published_registration_and_visibility(self):
        cache_updater = shutil.which("update-desktop-database")
        self.assertIsNotNone(cache_updater, "desktop-file-utils is required by the Linux test environment")
        tools = self.tmp / "failing-tool-path"
        tools.mkdir()
        failing = tools / "update-desktop-database"
        failing.write_text(
            "#!/bin/sh\n"
            '"$FROST_DESKTOP_REAL_UPDATER" "$@" || exit $?\n'
            f'test ! -f "$1/{DESKTOP}"\n',
            encoding="utf-8",
        )
        failing.chmod(0o755)
        original_defaults = self.defaults.read_bytes()
        with patch.dict(self.env, PATH=f"{tools}{os.pathsep}{self.env['PATH']}",
                        FROST_DESKTOP_REAL_UPDATER=cache_updater):
            result = self._install("register", expected=1)
        self.assertIn("update-desktop-database", result.stderr)
        self.assertFalse(self.application.exists())
        self.assertFalse(self.mime.exists())
        self.assertFalse(self.receipt.exists())
        self.assertEqual(self.defaults.read_bytes(), original_defaults)
        if gio := shutil.which("gio"):
            self.assertNotIn(DESKTOP, self._run(gio, "mime", "text/x-lua").stdout)
        self._install("register")
        self._install("unregister")

    def test_conflicting_or_modified_registration_is_preserved(self):
        self.application.write_text("foreign desktop entry\n", encoding="utf-8")
        self._install("register", expected=1)
        self.assertEqual(self.application.read_text(encoding="utf-8"), "foreign desktop entry\n")
        self.assertFalse(self.mime.exists())
        self.assertFalse(self.receipt.exists())
        self.application.unlink()

        self._install("register")
        other = self._package("other FrostIDE extraction")
        self._install("unregister", package=other, expected=1)
        self.assertTrue(self.receipt.is_file())
        with self.application.open("a", encoding="utf-8") as file:
            file.write("Comment=User change\n")
        result = self._install("unregister")
        self.assertIn("Preserved modified file", result.stdout)
        self.assertIn("Comment=User change", self.application.read_text(encoding="utf-8"))
        self.assertFalse(self.mime.exists())
        self.assertFalse(self.receipt.exists())


if __name__ == "__main__":
    unittest.main()
