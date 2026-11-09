# SPDX-License-Identifier: Apache-2.0
#
# Tests for tools/licensecheck.py (frost-licensecheck v0). The M0 exit's negative tests
# are here: the Qt Charts fixture and the GPL-3.0-only vcpkg port fixture must fail.
# Also covered: SPDX expression evaluation, Qt SBOM licences, the per-platform and
# per-context Qt module allowlist, the build-tree scan (Windows DLLs and Linux ELF
# DT_NEEDED), ADR exceptions, a missing installed tree, the SPDX 2.3 SBOM, the bundled-asset
# gate (ADR 0015) with its OpenType 'name'-table reader over synthetic fixtures, and the same gate
# over vendored sources (ADR 0023: the committed vendor lock and a synthetic one).

from __future__ import annotations

import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock

from tools_test_support import FIXTURES, REPO, TempDirTestCase, clean_env, run_tool

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

    def test_build_helper_exception_is_versioned_and_share_only(self) -> None:
        root = self.tmp / "installed"
        triplet = "x64-fixture"
        adr = self.tmp / "docs/adr/0075-signed-update-verification.md"
        _write(adr, "Build-only fixture approval\n")
        spdx = root / triplet / "share/vcpkg-make/vcpkg.spdx.json"
        listing = root / "vcpkg/info/vcpkg-make_2026-07-09_x64-fixture.list"
        package = {"SPDXID": "SPDXRef-port", "versionInfo": "2026-07-09#1",
                   "licenseConcluded": "LicenseRef-vcpkg-null"}

        def check() -> licensecheck.Report:
            _write(spdx, json.dumps({"packages": [package]}))
            report = licensecheck.Report()
            licensecheck.check_vcpkg(self.tmp, [root], report)
            return report

        _write(listing, f"{triplet}/\n{triplet}/share/\n{triplet}/share/vcpkg-make/wrappers/compile\n")
        accepted = check()
        self.assertEqual(accepted.violations, [])
        self.assertEqual(accepted.packages[0].declared, "LicenseRef-vcpkg-null")
        self.assertIn("not distributed", accepted.packages[0].comment)
        for version, declaration in (("2026-07-09#2", "LicenseRef-vcpkg-null"),
                                     ("2026-07-09#1", "GPL-2.0-or-later")):
            package.update(versionInfo=version, licenseConcluded=declaration)
            self.assertTrue(check().violations)
        package.update(versionInfo="2026-07-09#1", licenseConcluded="LicenseRef-vcpkg-null")
        for entry in (f"{triplet}/bin/helper.exe", f"{triplet}/lib/helper.lib",
                      f"{triplet}/share/vcpkg-make/../../lib/helper.lib"):
            _write(listing, entry + "\n")
            self.assertTrue(check().violations)
        listing.unlink()
        self.assertTrue(check().violations)


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
               "# Sql remains outside the runtime policy.\ntarget_link_libraries(a PRIVATE Qt6::Sql)\n")
        messages = self._messages()
        self.assertEqual(len(messages), 1)
        self.assertIn("src/CMakeLists.txt:2: Qt module Sql is not on the", messages[0])

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

    def test_private_module_package_has_the_module_licence(self) -> None:
        _write(self.repo / "src" / "CMakeLists.txt",
               "# QRhi lives in the private Gui package.\nfind_package(Qt6 COMPONENTS GuiPrivate)\n"
               "find_package(Qt6GuiPrivate)\ntarget_link_libraries(a PRIVATE Qt6::GuiPrivate)\n")
        self.assertEqual(self._messages(), [])

    def test_unknown_private_module_still_needs_an_adr(self) -> None:
        _write(self.repo / "src" / "CMakeLists.txt", "# Unknown.\nfind_package(Qt6 COMPONENTS FrobnicatePrivate)\n")
        messages = self._messages()
        self.assertEqual(len(messages), 1)
        self.assertIn("Frobnicate has no known licence", messages[0])

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
        _write(build / "src/app/Debug/tls/qschannelbackendd.dll", b"MZ")
        self.assertEqual(self._messages([build]), [])
        _write(build / "src/app/Debug/Qt6Sqld.dll", b"MZ")
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
        _write(build / "src/app/network", _elf(["libQt6Network.so.6"]))
        self.assertEqual(self._messages([build]), [])
        _write(build / "src/app/helper", _elf(["libQt6Sql.so.6"]))
        messages = self._messages([build])
        self.assertEqual(len(messages), 1)
        self.assertIn("Qt module Sql is not on the linux runtime allowlist", messages[0])

    @unittest.skipUnless(hasattr(os, "mkfifo"), "FIFOs are unavailable on this platform")
    def test_build_scan_skips_fifos_and_still_scans_symlinked_elf(self) -> None:
        build = self.tmp / "build-linux"
        target = build / "src/app/actual"
        _write(target, _elf(["libQt6Network.so.6"]))
        os.symlink(target.name, target.with_name("linked"))
        fifo = target.with_name("clr-debug-pipe-in")
        os.mkfifo(fifo)
        os.mkfifo(target.with_name("libQt6Charts.so.6"))
        os.symlink(fifo.name, fifo.with_name("pipe-link"))
        self.assertEqual(licensecheck.elf_needed(target.with_name("linked")), ["libQt6Network.so.6"])
        # A timeout bounds the regression: an ordinary blocking open of either FIFO hangs.
        script = ("import json,sys; from pathlib import Path; "
                  "sys.path.insert(0,sys.argv[1]); import licensecheck; "
                  "print(json.dumps([use.component for use in licensecheck.binary_qt_uses(Path(sys.argv[2]), "
                  "[Path(sys.argv[3])])]))")
        result = subprocess.run([sys.executable, "-B", "-c", script, str(REPO / "tools"),
                                 str(self.repo), str(build)], capture_output=True, text=True,
                                env=clean_env(), timeout=10, check=True)
        components = json.loads(result.stdout)
        self.assertIn("Network", components)
        self.assertNotIn("Charts", components)

    @unittest.skipUnless(hasattr(os, "mkfifo") and hasattr(os, "O_NONBLOCK"),
                         "nonblocking FIFO opens are unavailable on this platform")
    def test_elf_scan_does_not_block_if_regular_file_becomes_fifo_before_open(self) -> None:
        target = self.tmp / "replaced"
        _write(target, _elf(["libQt6Network.so.6"]))
        # Replace the file exactly between the regular-file check and open. Run in a child so a
        # broken blocking implementation fails on timeout instead of hanging the test suite.
        script = ("import os,sys; from pathlib import Path; sys.path.insert(0,sys.argv[1]); "
                  "import licensecheck; path=Path(sys.argv[2]); original=os.open; "
                  "exec('def swap(p, flags):\\n os.unlink(p)\\n os.mkfifo(p)\\n return original(p, flags)'); "
                  "licensecheck.os.open=swap; assert licensecheck.elf_needed(path)==[]")
        subprocess.run([sys.executable, "-B", "-c", script, str(REPO / "tools"), str(target)],
                       capture_output=True, text=True, env=clean_env(), timeout=10, check=True)


class VcpkgTest(TempDirTestCase):
    def test_missing_installed_tree_fails_when_the_manifest_has_dependencies(self) -> None:
        _write(self.tmp / "vcpkg.json", '{ "name": "x", "dependencies": [ "zlib" ] }\n')
        report = licensecheck.run(self.tmp, [self.tmp / "build" / "vcpkg_installed"], [], None)
        self.assertEqual(len(report.violations), 1)
        self.assertIn("no vcpkg installed tree", report.violations[0].message)

    def test_default_roots_cover_every_installed_tree(self) -> None:
        # The ASan preset has its own tree (build/vcpkg_installed-asan); both are checked,
        # and neither is mistaken for a build tree.
        for name in ("vcpkg_installed", "vcpkg_installed-asan", "win-clangcl"):
            (self.tmp / "build" / name).mkdir(parents=True)
        self.assertEqual([p.name for p in licensecheck.default_vcpkg_roots(self.tmp)],
                         ["vcpkg_installed", "vcpkg_installed-asan"])
        self.assertEqual([p.name for p in licensecheck.default_build_dirs(self.tmp)], ["win-clangcl"])

    def test_default_root_without_any_tree_is_still_checked(self) -> None:
        self.assertEqual(licensecheck.default_vcpkg_roots(self.tmp), [self.tmp / "build" / "vcpkg_installed"])

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


def _font(names: dict[int, str], platform: int = 3, encoding: int = 1) -> bytes:
    """A minimal sfnt holding only a 'name' table (UTF-16BE, or Mac Roman for platform 1)."""
    strings, records = b"", b""
    for name_id, text in sorted(names.items()):
        data = text.encode("mac_roman" if platform == 1 else "utf-16-be")
        records += struct.pack(">6H", platform, encoding, 0x409 if platform == 3 else 0, name_id, len(data),
                               len(strings))
        strings += data
    table = struct.pack(">3H", 0, len(names), 6 + len(records)) + records + strings
    return struct.pack(">IHHHH", 0x00010000, 1, 16, 0, 0) + b"name" + struct.pack(">III", 0, 28, len(table)) + table


class AssetGateTest(TempDirTestCase):
    """The bundled-asset gate (ADR 0015) over the synthetic licence/assets_* fixtures."""

    def _messages(self, fixture: str) -> list[str]:
        repo = self.copy_fixture(f"licence/{fixture}")
        report = licensecheck.Report()
        licensecheck.check_assets(repo, report)
        return [str(v) for v in report.violations]

    def test_ok_fixture_passes_through_the_cli(self) -> None:
        repo = self.copy_fixture("licence/assets_ok")
        out = self.tmp / "sbom.json"
        result = run_tool("licensecheck.py", "--repo", str(repo), "--qt-root", str(self.tmp / "no-qt"),
                          "--vcpkg-installed", str(self.tmp / "none"), "--sbom", str(out))
        self.assertNotIn("asset", result.stdout)
        packages = {p["name"]: p for p in json.loads(out.read_text(encoding="utf-8"))["packages"]}
        self.assertEqual(packages["Demo Font"]["licenseDeclared"], "OFL-1.1")
        self.assertEqual(packages["Demo Icons"]["licenseConcluded"], "ISC AND MIT")
        self.assertEqual(packages["Demo Font"]["checksums"], [{"algorithm": "SHA256", "checksumValue": "a" * 64}])
        self.assertEqual(packages["Demo Icons"]["downloadLocation"], "https://example.invalid/demo-icons-2.0.zip")

    def test_no_manifest_means_no_assets(self) -> None:
        self.assertEqual(self._messages("base"), [])

    def test_negative_fixtures(self) -> None:
        cases = {
            "assets_ok": [],
            "assets_bad_hash": ["resources/icons/demo/circle.svg:1: asset demo-icons: SHA-256 differs"],
            "assets_unlisted": ["resources/icons/demo/extra.svg:1: asset demo-icons: file is not listed"],
            "assets_gpl": ["asset demo-icons: licence 'GPL-3.0-only' is not on the allowlist; needs an ADR",
                           "no known licence text marker for GPL-3.0-only"],
            "assets_rfn_modified": ["asset demo-font: a modified font may not keep its Reserved Font Names"],
            "assets_missing_mit_text": ["resources/icons/demo/LICENSE:1: asset demo-icons: the licence file lacks "
                                        "the MIT text"],
        }
        for fixture, expected in cases.items():
            with self.subTest(fixture=fixture):
                messages = self._messages(fixture)
                self.assertEqual(len(messages), len(expected), messages)
                for fragment, message in zip(expected, messages):
                    self.assertIn(fragment, message)

    def test_missing_file_adr_and_invalid_manifest(self) -> None:
        repo = self.copy_fixture("licence/assets_ok")
        (repo / "resources" / "fonts" / "demo" / "OFL.txt").unlink()
        (repo / "docs" / "adr" / "0015-demo.txt").unlink()
        report = licensecheck.Report()
        licensecheck.check_assets(repo, report)
        messages = [str(v) for v in report.violations]
        self.assertTrue(any("OFL.txt:1: asset demo-font: listed file is missing" in m for m in messages), messages)
        self.assertEqual(sum("ADR docs/adr/0015-demo.txt does not exist" in m for m in messages), 2, messages)
        (repo / "resources" / "assets.lock.json").write_text('{"schemaVersion": 2}', encoding="utf-8")
        report = licensecheck.Report()
        licensecheck.check_assets(repo, report)
        self.assertEqual(len(report.violations), 1)
        self.assertIn("invalid asset manifest", report.violations[0].message)

    def _with_font(self, data: bytes, licence_preamble: str | None = None, **asset_changes) -> list[str]:
        """The demo font's violations with `data` as its TTF, `licence_preamble` (when given) put
        before its OFL.txt (both re-pinned) and `asset_changes` applied to its manifest entry."""
        self._cases = getattr(self, "_cases", 0) + 1
        repo = self.tmp / f"case{self._cases}"
        shutil.copytree(FIXTURES / "licence" / "assets_ok", repo)
        font = repo / "resources" / "fonts" / "demo" / "Demo-Regular.ttf"
        font.write_bytes(data)
        manifest = repo / "resources" / "assets.lock.json"
        document = json.loads(manifest.read_text(encoding="utf-8"))
        document["assets"][0]["files"][0]["sha256"] = hashlib.sha256(data).hexdigest()
        if licence_preamble is not None:
            licence = repo / "resources" / "fonts" / "demo" / "OFL.txt"
            licence.write_bytes(licence_preamble.encode("utf-8") + licence.read_bytes())
            document["assets"][0]["files"][1]["sha256"] = hashlib.sha256(licence.read_bytes()).hexdigest()
        document["assets"][0].update(asset_changes)
        manifest.write_text(json.dumps(document), encoding="utf-8")
        report = licensecheck.Report()
        licensecheck.check_assets(repo, report)
        return [str(v) for v in report.violations]

    def test_font_name_table_checks(self) -> None:
        holder = "Copyright 2026 The Demo Font Authors"
        # The licence URL alone (nameID 14) is enough.
        self.assertEqual(self._with_font(_font({0: holder, 14: "https://scripts.sil.org/OFL"})), [])
        # Mac Roman records are read too.
        self.assertEqual(self._with_font(_font({0: holder, 13: "SIL Open Font License, Version 1.1"}, 1, 0)), [])
        messages = self._with_font(_font({0: holder, 13: "All rights reserved"}))
        self.assertEqual(len(messages), 1, messages)
        self.assertIn("names no SIL Open Font License 1.1", messages[0])
        messages = self._with_font(_font({0: "Copyright 2026 Somebody Else", 14: "https://openfontlicense.org"}))
        self.assertEqual(len(messages), 1, messages)
        self.assertIn("does not name 'The Demo Font Authors'", messages[0])
        # A font without the licence fields may be recorded as such (the licence file still applies).
        self.assertEqual(self._with_font(_font({0: "x"}), nameTableLicence=False), [])
        messages = self._with_font(b"not a font at all")
        self.assertEqual(len(messages), 1, messages)
        self.assertIn("unreadable font", messages[0])
        # The holder must appear as whole words: "The Demo Font Authors" is not in "...Authorship".
        authorship = _font({0: "Copyright 2026 The Demo Font Authorship", 14: "https://openfontlicense.org"})
        messages = self._with_font(authorship)
        self.assertEqual(len(messages), 1, messages)
        self.assertIn("does not name", messages[0])
        # A font over the size limit is refused before its bytes are hashed or parsed.
        with mock.patch.object(licensecheck, "_MAX_FONT_BYTES", 16):
            messages = self._with_font(_font({0: holder, 14: "https://openfontlicense.org"}))
        self.assertEqual(len(messages), 1, messages)
        self.assertIn("unreadable: larger than", messages[0])

    def test_unpinned_archive_is_a_violation(self) -> None:
        holder = "Copyright 2026 The Demo Font Authors"
        messages = self._with_font(_font({0: holder, 14: "https://openfontlicense.org"}), sha256="")
        self.assertEqual(len(messages), 1, messages)
        self.assertIn("the archive SHA-256 is not pinned", messages[0])

    def test_font_names_rejects_malformed_tables(self) -> None:
        good = _font({0: "Copyright 2026 X", 1: "Demo"})
        self.assertEqual(licensecheck.font_names(good)[1], ["Demo"])
        bad = {
            "truncated": good[:40],
            "no name table": good[:12] + b"glyf" + good[16:],
            "table outside": good[:24] + struct.pack(">I", 1 << 20) + good[28:],
            "record outside": good[:28 + 6 + 8] + struct.pack(">H", 0xFFFF) + good[28 + 6 + 10:],
            "collection": b"ttcf" + good[4:],
            "odd utf-16": _font({0: "x"})[:28 + 6 + 8] + struct.pack(">H", 1) + _font({0: "x"})[28 + 6 + 10:],
        }
        for label, data in bad.items():
            with self.subTest(label):
                with self.assertRaises(licensecheck.FontError):
                    licensecheck.font_names(data)
        # Every truncation of a valid font is rejected cleanly, never with another exception.
        for size in range(len(good)):
            try:
                licensecheck.font_names(good[:size])
            except licensecheck.FontError:
                pass

    def test_copyright_holder(self) -> None:
        cases = {
            "Copyright (c) 2016 The Inter Project Authors (https://github.com/rsms/inter)": "The Inter Project Authors",
            "Copyright 2020 The JetBrains Mono Project Authors (https://github.com/JetBrains/JetBrainsMono)":
                "The JetBrains Mono Project Authors",
            "Copyright (c) 2013-present Cole Bemis": "Cole Bemis",
            "Copyright © 2026 Lucide Icons and Contributors": "Lucide Icons and Contributors",
            "Copyright 2016 3M Company": "3M Company",
            "Copyright (c) 2019, 2020 - 2022 Foo Bar.": "Foo Bar",
        }
        for line, holder in cases.items():
            self.assertEqual(licensecheck.copyright_holder(line), holder)

    def test_names_holder_matches_whole_words(self) -> None:
        self.assertTrue(licensecheck.names_holder("Copyright 2016 the inter project authors",
                                                  "The Inter Project Authors"))
        self.assertFalse(licensecheck.names_holder("Copyright 2016 Themis Ltd", "The"))
        self.assertFalse(licensecheck.names_holder("anything", ""))

    def test_committed_assets_pass(self) -> None:
        report = licensecheck.Report()
        licensecheck.check_assets(REPO, report)
        self.assertEqual([str(v) for v in report.violations], [])
        # Lucide and the UI/editor fonts of ADR 0015 decision 9, plus the eight editor font families its
        # 2026-11-07 amendment records (bundled by d540658). A new asset still needs this list and the ADR.
        self.assertEqual({p.name for p in report.packages},
                         {"Lucide", "Inter", "JetBrains Mono", "Fira Code", "Cascadia Code", "Source Code Pro",
                          "IBM Plex Mono", "Inconsolata", "Intel One Mono", "Commit Mono", "Fantasque Sans Mono"})
        fonts = [p for p in report.packages if p.name != "Lucide"]
        for package in fonts:
            self.assertEqual(package.declared, "OFL-1.1", package.name)
            self.assertTrue(package.sha256, package.name)
        # Every font's Reserved Font Names, as its licence file declares them (OFL-1.1 condition 3).
        _document, assets = licensecheck.asset_fetch.load_manifest(REPO / licensecheck.ASSET_MANIFEST)
        reserved = {a["id"]: a["reservedFontNames"] for a in assets if a["reservedFontNames"]}
        self.assertEqual(reserved, {"cascadia-code": ["Cascadia Code"], "source-code-pro": ["Source"],
                                    "ibm-plex-mono": ["Plex"], "intel-one-mono": ["Intel"]})

    def test_declared_reserved_font_names(self) -> None:
        header = licensecheck.LICENCE_MARKERS["OFL-1.1"]
        body = f"\n{header} - 26 February 2007\n\"Reserved Font Name\" refers to any names specified as such\n"
        cases = {
            "Copyright (c) 2019 - Present, Microsoft Corporation,\nwith Reserved Font Name Cascadia Code.\n":
                ["Cascadia Code"],
            "Copyright 2017 IBM Corp. with Reserved Font Name \"Plex\"\n": ["Plex"],
            "© 2023 Adobe, with Reserved Font Name 'Source'. All Rights Reserved. Source is a trademark.\n":
                ["Source"],
            "Copyright 2020 X, with Reserved Font Names “A”, “B” and “C”.\n": ["A", "B", "C"],
            "Copyright 2020 X, with Reserved Font Names Alpha, Beta and Gamma.\n": ["Alpha", "Beta", "Gamma"],
            "Copyright 2020 The Demo Authors\n": [],
        }
        for preamble, expected in cases.items():
            with self.subTest(preamble=preamble):
                self.assertEqual(licensecheck.declared_reserved_font_names(preamble + body, header), expected)

    def test_undeclared_reserved_font_name_is_a_violation(self) -> None:
        holder = "Copyright 2026 The Demo Font Authors"
        font = _font({0: holder, 14: "https://scripts.sil.org/OFL"})
        preamble = f"{holder}, with Reserved Font Name \"Demo\".\n"
        messages = self._with_font(font, preamble, reservedFontNames=[])
        self.assertEqual(len(messages), 1, messages)
        self.assertIn("OFL.txt:1: asset demo-font: the licence file declares the Reserved Font Name 'Demo', "
                      "which reservedFontNames", messages[0])
        # Recording it (in any case) passes; the recorded name is then bound by the modified-font rule.
        self.assertEqual(self._with_font(font, preamble, reservedFontNames=["demo"]), [])
        messages = self._with_font(font, preamble, reservedFontNames=["Demo"], modified=True)
        self.assertEqual(len(messages), 1, messages)
        self.assertIn("a modified font may not keep its Reserved Font Names", messages[0])
        # A licence without a declaration requires none.
        self.assertEqual(self._with_font(font), [])

    def test_committed_vendored_sources_pass(self) -> None:
        report = licensecheck.Report()
        licensecheck.check_assets(REPO, report, manifest_rel=licensecheck.VENDOR_MANIFEST)
        self.assertEqual([str(v) for v in report.violations], [])
        self.assertEqual(len(report.packages), 10)
        self.assertIn("tree-sitter", {p.name for p in report.packages})
        self.assertIn("tree-sitter-lua", {p.name for p in report.packages})
        self.assertIn("libvterm", {p.name for p in report.packages})
        for package in report.packages:
            self.assertEqual(package.declared, "MIT")
            # libvterm comes from its author's release tarball (no git host, ADR 0051); the rest from GitHub.
            prefix = "pkg:generic/libvterm@0.3.3?" if package.name == "libvterm" else "pkg:github/"
            self.assertTrue(package.purl and package.purl.startswith(prefix), package.name)
            self.assertTrue(package.spdx_id.startswith("SPDXRef-Vendored-"), package.spdx_id)

    def test_vendored_gate_reports_unlisted_files_and_licences(self) -> None:
        # A synthetic vendor lock: one component with a nested file, its MIT licence and its ADR.
        mit = b"MIT License\n\nPermission is hereby granted, free of charge, to any person (synthetic)\n"
        source = b"int demo(void) { return 0; }\n"
        dest = self.tmp / "third_party" / "demo"
        (dest / "src").mkdir(parents=True)
        (dest / "LICENSE").write_bytes(mit)
        (dest / "src" / "demo.c").write_bytes(source)
        (self.tmp / "docs").mkdir()
        (self.tmp / "docs" / "adr.md").write_text("synthetic\n", encoding="utf-8")
        component = {
            "id": "demo", "name": "demo", "version": "1.0", "commit": "1" * 40, "purl": "pkg:github/demo/demo@v1.0",
            "url": "https://example.invalid/demo.tar.gz", "archive": "demo.tar.gz", "sha256": "a" * 64,
            "crossCheck": "synthetic", "licence": "MIT", "copyright": ["Copyright (c) 2026 Demo"],
            "reservedFontNames": [], "modified": False, "homepage": "https://example.invalid",
            "dest": "third_party/demo", "licenceFile": "LICENSE", "adr": "docs/adr.md",
            "files": [{"member": "demo/LICENSE", "path": "LICENSE", "sha256": hashlib.sha256(mit).hexdigest()},
                      {"member": "demo/src/demo.c", "path": "src/demo.c",
                       "sha256": hashlib.sha256(source).hexdigest()}],
        }
        lock = self.tmp / "third_party" / "vendor.lock.json"

        def messages(files: list[str]) -> list[str]:
            lock.write_text(json.dumps({"schemaVersion": 1, "components": [component]}), encoding="utf-8")
            report = licensecheck.Report()
            licensecheck.check_assets(self.tmp, report, files, licensecheck.VENDOR_MANIFEST)
            return [str(v) for v in report.violations]

        listed = ["third_party/demo/LICENSE", "third_party/demo/src/demo.c"]
        self.assertEqual(messages(listed), [])
        unlisted = messages(listed + ["third_party/demo/src/extra.c"])
        self.assertEqual(len(unlisted), 1)
        self.assertIn("vendored demo: file is not listed in third_party/vendor.lock.json", unlisted[0])
        component["licence"] = "GPL-3.0-only"
        self.assertTrue(any("not on the allowlist" in m for m in messages(listed)))


class ConPtySdkPinTest(TempDirTestCase):
    def test_pin_enters_sbom_and_requires_allowed_licence_and_notice(self) -> None:
        pin = {"version": "1", "url": "https://example.invalid/conpty", "sha256": "a" * 64,
               "license": "MIT", "copyright": "Copyright Demo", "adr": "docs/adr.md"}
        _write(self.tmp / "sdk/sdk.lock.json", json.dumps({"windows": {"conpty": pin}}))
        _write(self.tmp / "docs/adr.md", "accepted")
        _write(self.tmp / "NOTICE", "Copyright Demo. Permission is hereby granted, free of charge")
        report = licensecheck.Report()
        licensecheck.check_sdk_conpty(self.tmp, report)
        self.assertEqual(report.violations, [])
        self.assertEqual(report.packages[0].sha256, "a" * 64)
        pin["license"] = "GPL-3.0-only"
        _write(self.tmp / "sdk/sdk.lock.json", json.dumps({"windows": {"conpty": pin}}))
        report = licensecheck.Report()
        licensecheck.check_sdk_conpty(self.tmp, report)
        self.assertTrue(report.violations)


class LuaLsSdkPinTest(TempDirTestCase):
    def test_both_pins_enter_the_sbom_and_need_an_allowed_licence_and_notice(self) -> None:
        pin = {"version": "3.19.1", "url": "https://example.invalid/luals", "sha256": "b" * 64,
               "license": "MIT", "copyright": "Copyright (c) 2018 Demo", "adr": "docs/adr.md"}
        _write(self.tmp / "sdk/sdk.lock.json", json.dumps({"windows": {"luals": pin}, "linux": {"luals": pin}}))
        _write(self.tmp / "docs/adr.md", "accepted")
        _write(self.tmp / "NOTICE", "Copyright (c) 2018 Demo. Permission is hereby granted, free of charge")
        report = licensecheck.Report()
        licensecheck.check_sdk_luals(self.tmp, report)
        self.assertEqual(report.violations, [])
        self.assertEqual([p.comment for p in report.packages], ["Bundled LuaLS (windows)", "Bundled LuaLS (linux)"])
        for change in ({"license": "GPL-3.0-only"}, {"sha256": "short"}, {"adr": "docs/none.md"},
                       {"copyright": "Copyright Someone Else"}):
            broken = dict(pin, **change)
            _write(self.tmp / "sdk/sdk.lock.json", json.dumps({"linux": {"luals": broken}}))
            report = licensecheck.Report()
            licensecheck.check_sdk_luals(self.tmp, report)
            self.assertEqual(len(report.violations), 1, change)
            self.assertIn("LuaLS linux pin", str(report.violations[0]))

    def test_the_repository_pins_pass(self) -> None:
        report = licensecheck.Report()
        licensecheck.check_sdk_luals(REPO, report)
        self.assertEqual(report.violations, [])
        self.assertEqual(len(report.packages), 2)


class ProjectHostReferenceTest(TempDirTestCase):
    """SLN-6 (ADR 0082): the ProjectHost helper references only allowlisted, installed assemblies."""

    def _check(self, references: str) -> list[str]:
        _write(self.tmp / licensecheck.PROJECT_HOST_CSPROJ,
               f'<Project Sdk="Microsoft.NET.Sdk"><ItemGroup>{references}</ItemGroup></Project>')
        for _, adr in licensecheck.PROJECT_HOST_REFERENCES.values():
            _write(self.tmp / adr, "accepted")
        report = licensecheck.Report()
        licensecheck.check_project_host(self.tmp, report)
        return [violation.message for violation in report.violations]

    def test_installed_allowlisted_references_pass_and_others_are_refused(self) -> None:
        good = ('<Reference Include="System.Xml" />'
                '<Reference Include="Microsoft.VisualStudio.SolutionPersistence" '
                'HintPath="$(VisualStudioMSBuildDir)/../Microsoft.VisualStudio.SolutionPersistence.dll" Private="false" />')
        self.assertEqual(self._check(good), [])
        for bad, expected in (
                ('<Reference Include="Newtonsoft.Json" HintPath="$(VisualStudioMSBuildDir)/Newtonsoft.Json.dll" '
                 'Private="false" />', "not on the ProjectHost allowlist"),
                ('<Reference Include="Microsoft.Build" HintPath="$(VisualStudioMSBuildDir)/Microsoft.Build.dll" />',
                 "Private=\"false\""),
                ('<Reference Include="Microsoft.Build" HintPath="lib/Microsoft.Build.dll" Private="false" />',
                 "installed Visual Studio"),
                ('<PackageReference Include="Microsoft.Build" Version="17.0.0" />', "package reference")):
            messages = self._check(bad)
            self.assertEqual(len(messages), 1, bad)
            self.assertIn(expected, messages[0])

    def test_the_repository_helper_passes(self) -> None:
        report = licensecheck.Report()
        licensecheck.check_project_host(REPO, report)
        self.assertEqual(report.violations, [])


class ManagedSdkPinTest(TempDirTestCase):
    def test_transitive_licences_and_notices_are_checked(self) -> None:
        for relative in ("sdk/sdk.lock.json", "sdk/managed-dependencies.json", "NOTICE", "docs/adr/0064-managed-language.md"):
            source = REPO / relative
            _write(self.tmp / relative, source.read_bytes())
        shutil.copytree(REPO / "third_party/managed-notices", self.tmp / "third_party/managed-notices")
        report = licensecheck.Report()
        licensecheck.check_sdk_managed(self.tmp, report)
        self.assertEqual(report.violations, [])
        self.assertEqual(len(report.packages), 61)
        manifest_path = self.tmp / "sdk/managed-dependencies.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["packages"][0]["license"] = "GPL-3.0-only"
        data = (json.dumps(manifest) + "\n").encode()
        manifest_path.write_bytes(data)
        lock_path = self.tmp / "sdk/sdk.lock.json"
        lock = json.loads(lock_path.read_text(encoding="utf-8"))
        lock["managedServer"]["manifestSha256"] = hashlib.sha256(data).hexdigest()
        lock_path.write_text(json.dumps(lock), encoding="utf-8")
        report = licensecheck.Report()
        licensecheck.check_sdk_managed(self.tmp, report)
        self.assertTrue(any("not on allowlist" in str(v) for v in report.violations))

    def test_changed_notice_fails_even_when_manifest_is_pinned(self) -> None:
        for relative in ("sdk/sdk.lock.json", "sdk/managed-dependencies.json", "NOTICE", "docs/adr/0064-managed-language.md"):
            _write(self.tmp / relative, (REPO / relative).read_bytes())
        shutil.copytree(REPO / "third_party/managed-notices", self.tmp / "third_party/managed-notices")
        (self.tmp / "third_party/managed-notices/ionide-license.txt").write_text("changed", encoding="utf-8")
        report = licensecheck.Report()
        licensecheck.check_sdk_managed(self.tmp, report)
        self.assertTrue(any("notice hash mismatch" in str(v) for v in report.violations))


if __name__ == "__main__":
    unittest.main()
