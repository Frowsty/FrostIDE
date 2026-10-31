# SPDX-License-Identifier: Apache-2.0
#
# Tests for tools/licensecheck.py (frost-licensecheck v0). The M0 exit's negative tests
# are here: the Qt Charts fixture and the GPL-3.0-only vcpkg port fixture must fail.
# Also covered: SPDX expression evaluation, Qt SBOM licences, the per-platform and
# per-context Qt module allowlist, the build-tree scan (Windows DLLs and Linux ELF
# DT_NEEDED), ADR exceptions, a missing installed tree and the SPDX 2.3 SBOM.

from __future__ import annotations

import json
import struct
import unittest
from pathlib import Path
from unittest import mock

from tools_test_support import TempDirTestCase, clean_env, run_tool

import licensecheck


def _write(path: Path, data: str | bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(data, bytes):
        path.write_bytes(data)
    else:
        path.write_text(data, encoding="utf-8", newline="\n")


def _elf(needed: list[str]) -> bytes:
    """A minimal 64-bit little-endian ELF shared object with DT_NEEDED entries."""
    strings = b"\0"
    offsets = []
    for name in needed:
        offsets.append(len(strings))
        strings += name.encode() + b"\0"
    dynamic = b"".join(struct.pack("<qQ", 1, off) for off in offsets) + struct.pack("<qQ", 0, 0)
    str_offset = 64
    dyn_offset = str_offset + len(strings)
    sh_offset = dyn_offset + len(dynamic)
    header = b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8)
    header += struct.pack("<HHIQQQIHHHHHH", 3, 62, 1, 0, 0, sh_offset, 0, 64, 0, 0, 64, 3, 0)
    sections = bytes(64)  # SHN_UNDEF
    sections += struct.pack("<IIQQQQIIQQ", 0, 3, 0, 0, str_offset, len(strings), 0, 0, 1, 0)  # SHT_STRTAB
    sections += struct.pack("<IIQQQQIIQQ", 0, 6, 0, 0, dyn_offset, len(dynamic), 1, 0, 8, 16)  # SHT_DYNAMIC
    return header + strings + dynamic + sections


class ExpressionTest(unittest.TestCase):
    def test_simple_and_with_expressions_match_exactly(self) -> None:
        allowed = licensecheck.ALLOWLIST
        self.assertTrue(licensecheck.expression_allowed("MIT", allowed))
        self.assertTrue(licensecheck.expression_allowed("Apache-2.0 WITH LLVM-exception", allowed))
        self.assertTrue(licensecheck.expression_allowed("BSD-3-Clause WITH PCRE2-exception", allowed))
        self.assertFalse(licensecheck.expression_allowed("MIT WITH LLVM-exception", allowed))
        self.assertFalse(licensecheck.expression_allowed("GPL-3.0-or-later", allowed))
        self.assertFalse(licensecheck.expression_allowed("mit", allowed))

    def test_compound_expressions(self) -> None:
        allowed = licensecheck.ALLOWLIST
        self.assertTrue(licensecheck.expression_allowed("MIT OR GPL-2.0-only", allowed))
        self.assertFalse(licensecheck.expression_allowed("MIT AND Unicode-3.0", allowed))
        self.assertTrue(licensecheck.expression_allowed("(MIT AND Zlib) OR GPL-3.0-only", allowed))
        self.assertFalse(licensecheck.expression_allowed("MIT AND (Zlib OR GPL-3.0-only) AND OFL-1.0", allowed))

    def test_lgpl_only_for_qt(self) -> None:
        qt = "LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only"
        self.assertFalse(licensecheck.expression_allowed("LGPL-3.0-only", licensecheck.ALLOWLIST))
        self.assertTrue(licensecheck.expression_allowed(qt, licensecheck.QT_ALLOWLIST))
        self.assertFalse(licensecheck.expression_allowed("LicenseRef-Qt-Commercial OR GPL-3.0-only",
                                                         licensecheck.QT_ALLOWLIST))

    def test_malformed_expressions_raise(self) -> None:
        for text in ("", "MIT OR", "(MIT", "MIT WITH", "MIT )", "MIT AND OR Zlib"):
            with self.assertRaises(licensecheck.ExpressionError, msg=text):
                licensecheck.parse_expression(text)


class NegativeFixtureTest(TempDirTestCase):
    """The M0 exit criterion: licence negative tests fail as designed."""

    def test_qt_charts_fixture_fails(self) -> None:
        repo = self.copy_fixture("licence/qt_charts")
        result = run_tool("licensecheck.py", "--repo", str(repo), "--qt-root", str(self.tmp / "no-qt"))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("CMakeLists.txt:3: Qt module Charts", result.stdout)
        self.assertIn("GPL-only", result.stdout)

    def test_gpl_port_fixture_fails(self) -> None:
        repo = self.copy_fixture("licence/gpl_port")
        result = run_tool("licensecheck.py", "--repo", str(repo), "--qt-root", str(self.tmp / "no-qt"))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("build/vcpkg_installed/x64-fixture/share/gplport/vcpkg.spdx.json:1:", result.stdout)
        self.assertIn("'GPL-3.0-only'", result.stdout)
        self.assertIn("needs an ADR", result.stdout)

    def test_adr_exception_requires_the_exact_expression(self) -> None:
        repo = self.copy_fixture("licence/gpl_port")
        roots = [repo / "build" / "vcpkg_installed"]
        approved = {"gplport": ("GPL-3.0-only", "docs/adr/9999-example.md")}
        with mock.patch.object(licensecheck, "ADR_EXCEPTIONS", approved):
            self.assertEqual(licensecheck.run(repo, roots, [], None).violations, [])
        with mock.patch.object(licensecheck, "ADR_EXCEPTIONS", {"gplport": ("GPL-2.0-only", "docs/adr/x.md")}):
            self.assertEqual(len(licensecheck.run(repo, roots, [], None).violations), 1)


class QtUsageTest(TempDirTestCase):
    def setUp(self) -> None:
        super().setUp()
        self.repo = self.copy_fixture("licence/base")
        self.qt = self.repo / "qt"

    def _messages(self, build_dirs: list[Path] | None = None) -> list[str]:
        report = licensecheck.run(self.repo, [self.repo / "build" / "vcpkg_installed"], build_dirs or [], self.qt)
        return [str(v) for v in report.violations]

    def test_base_fixture_passes(self) -> None:
        result = run_tool("licensecheck.py", "--repo", str(self.repo), "--qt-root", str(self.qt))
        self.assertEqual(result.stdout, "")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_module_outside_the_runtime_allowlist_fails(self) -> None:
        _write(self.repo / "src" / "CMakeLists.txt",
               "# Uses Qt Network, which PLAN.md bans.\ntarget_link_libraries(a PRIVATE Qt6::Network)\n")
        messages = self._messages()
        self.assertEqual(len(messages), 1)
        self.assertIn("src/CMakeLists.txt:2: Qt module Network is not on the", messages[0])

    def test_test_and_svg_modules_only_in_their_contexts(self) -> None:
        _write(self.repo / "src" / "CMakeLists.txt",
               "# Test and Svg outside their contexts.\nfind_package(Qt6 COMPONENTS Test Svg)\n")
        messages = self._messages()
        self.assertEqual(len(messages), 2)
        self.assertTrue(any("Qt module Test" in m for m in messages))
        self.assertTrue(any("Qt module Svg" in m for m in messages))

    def test_gpl_only_module_from_the_qt_sbom_fails(self) -> None:
        sbom = {"packages": [{"SPDXID": "SPDXRef-Package-qtcanvaspainter-qt-module-CanvasPainter-0123456789ab",
                              "licenseDeclared": "LicenseRef-Qt-Commercial OR GPL-3.0-only"}]}
        _write(self.qt / "sbom" / "qtcanvaspainter-6.12.0.spdx.json", json.dumps(sbom))
        _write(self.repo / "src" / "CMakeLists.txt", "# Canvas Painter.\nfind_package(Qt6 COMPONENTS CanvasPainter)\n")
        messages = self._messages()
        self.assertEqual(len(messages), 1)
        self.assertIn("Qt module CanvasPainter is licensed 'LicenseRef-Qt-Commercial OR GPL-3.0-only'", messages[0])

    def test_malformed_qt_sbom_licence_is_reported(self) -> None:
        sbom = {"packages": [{"SPDXID": "SPDXRef-Package-qtbroken-qt-module-Broken-0123456789ab",
                              "licenseDeclared": "LGPL-3.0-only OR"}]}
        _write(self.qt / "sbom" / "qtbroken-6.12.0.spdx.json", json.dumps(sbom))
        _write(self.repo / "src" / "CMakeLists.txt", "# Broken.\nfind_package(Qt6 COMPONENTS Broken)\n")
        messages = self._messages()
        self.assertEqual(len(messages), 1)
        self.assertIn("src/CMakeLists.txt:2: Qt module Broken", messages[0])
        self.assertIn("needs an ADR", messages[0])

    def test_unknown_module_needs_an_adr(self) -> None:
        _write(self.repo / "src" / "CMakeLists.txt", "# Unknown.\ntarget_link_libraries(a PRIVATE Qt6::Frobnicate)\n")
        messages = self._messages()
        self.assertEqual(len(messages), 1)
        self.assertIn("Frobnicate has no known licence", messages[0])

    def test_fixture_directories_are_not_scanned(self) -> None:
        _write(self.repo / "tests" / "x" / "fixtures" / "CMakeLists.txt", "find_package(Qt6 COMPONENTS Charts)\n")
        self.assertEqual(self._messages(), [])

    def test_windows_build_tree_scan(self) -> None:
        build = self.tmp / "build-win"
        for rel in ("src/app/Debug/Qt6Cored.dll", "src/app/Debug/Qt6Widgetsd.dll",
                    "src/app/Debug/platforms/qwindowsd.dll", "src/app/Debug/platforms/qoffscreend.dll",
                    "tests/app/Debug/Qt6Testd.dll", "tests/app/Debug/Qt6Widgetsd.dll"):
            _write(build / rel, b"MZ")
        self.assertEqual(self._messages([build]), [])
        _write(build / "src/app/Debug/Qt6Networkd.dll", b"MZ")
        _write(build / "src/app/Debug/Qt6DBusd.dll", b"MZ")
        _write(build / "src/app/Debug/platforms/qminimald.dll", b"MZ")
        messages = self._messages([build])
        self.assertEqual(len(messages), 3, messages)
        self.assertTrue(any("Qt module DBus is not on the windows runtime allowlist" in m for m in messages))
        self.assertTrue(any("Qt plugin platforms/qminimal" in m for m in messages))

    def test_linux_build_tree_scan_reads_dt_needed(self) -> None:
        build = self.tmp / "build-linux"
        _write(build / "src/app/frost", _elf(["libQt6Widgets.so.6", "libQt6DBus.so.6", "libc.so.6"]))
        _write(build / "src/app/platforms/libqxcb.so", b"\x7fELF")
        self.assertEqual(licensecheck.elf_needed(build / "src/app/frost"),
                         ["libQt6Widgets.so.6", "libQt6DBus.so.6", "libc.so.6"])
        self.assertEqual(self._messages([build]), [])
        _write(build / "src/app/helper", _elf(["libQt6Network.so.6"]))
        messages = self._messages([build])
        self.assertEqual(len(messages), 1)
        self.assertIn("Qt module Network is not on the linux runtime allowlist", messages[0])


class VcpkgTest(TempDirTestCase):
    def test_missing_installed_tree_fails_when_the_manifest_has_dependencies(self) -> None:
        _write(self.tmp / "vcpkg.json", '{ "name": "x", "dependencies": [ "zlib" ] }\n')
        report = licensecheck.run(self.tmp, [self.tmp / "build" / "vcpkg_installed"], [], None)
        self.assertEqual(len(report.violations), 1)
        self.assertIn("no vcpkg installed tree", report.violations[0].message)

    def test_port_without_a_licence_needs_an_adr(self) -> None:
        root = self.tmp / "installed"
        _write(root / "x64-t" / "share" / "nolicence" / "vcpkg.spdx.json",
               json.dumps({"packages": [{"SPDXID": "SPDXRef-port", "name": "nolicence", "versionInfo": "1",
                                         "licenseConcluded": "NOASSERTION", "licenseDeclared": "NOASSERTION"}]}))
        report = licensecheck.run(self.tmp, [root], [], None)
        self.assertEqual(len(report.violations), 1)
        self.assertIn("declares no licence; needs an ADR", report.violations[0].message)


class SbomTest(TempDirTestCase):
    def test_sbom_is_spdx_2_3_with_every_package(self) -> None:
        repo = self.copy_fixture("licence/base")
        out = self.tmp / "out" / "sbom.spdx.json"
        result = run_tool("licensecheck.py", "--repo", str(repo), "--qt-root", str(repo / "qt"), "--sbom", str(out),
                          env=clean_env(SOURCE_DATE_EPOCH="1793404800"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        document = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(document["spdxVersion"], "SPDX-2.3")
        self.assertEqual(document["dataLicense"], "CC0-1.0")
        self.assertEqual(document["creationInfo"]["created"], "2026-10-31T00:00:00Z")
        packages = {p["name"]: p for p in document["packages"]}
        self.assertEqual(packages["FrostIDE"]["licenseDeclared"], "Apache-2.0")
        self.assertEqual(packages["zlib"]["licenseDeclared"], "Zlib")
        self.assertEqual(packages["llvmish"]["licenseDeclared"], "Apache-2.0 WITH LLVM-exception")
        self.assertEqual(packages["pcre2"]["licenseDeclared"], "BSD-3-Clause WITH PCRE2-exception")
        self.assertIn("LGPL-3.0-only", packages["Qt Widgets"]["licenseDeclared"])
        self.assertEqual(packages["Qt Widgets"]["licenseConcluded"], "LGPL-3.0-only")
        self.assertEqual(packages["Qt Widgets"]["versionInfo"], "6.12.0")
        ids = {p["SPDXID"] for p in document["packages"]} | {"SPDXRef-DOCUMENT"}
        self.assertEqual(len(ids), len(document["packages"]) + 1)
        for relation in document["relationships"]:
            self.assertIn(relation["spdxElementId"], ids)
            self.assertIn(relation["relatedSpdxElement"], ids)
        self.assertTrue(any(r["relationshipType"] == "TEST_DEPENDENCY_OF" and
                            r["spdxElementId"] == packages["Qt Test"]["SPDXID"] for r in document["relationships"]))
        defined = {info["licenseId"] for info in document.get("hasExtractedLicensingInfos", [])}
        self.assertIn("LicenseRef-Qt-Commercial", defined)


if __name__ == "__main__":
    unittest.main()
