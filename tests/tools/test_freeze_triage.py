# SPDX-License-Identifier: Apache-2.0
#
# Tests for tools/freeze_triage.py (FND-9b): the check gate (exit 1 exactly when a report is over the
# limit, ongoing reports included), report reading (a file that is not frost-freeze-report/1 is a
# usage error, 2), the bucketing of symbolized samples (innermost FrostIDE function; a stall below the
# event loop names the innermost Qt-level frame; unsymbolized FrostIDE frames keep their reason), the
# PE export-table fallback over a synthetic PE32+ image built here (its export directory holds two
# named exports and a forwarder), and MSVC-decorated export names. On Windows the export reader is
# also compared with llvm-readobj --coff-exports (the oracle) on this tree's frost_app.dll when both
# are there; otherwise that test is skipped and says why.

from __future__ import annotations

import json
import os
import re
import shutil
import struct
import subprocess
import sys
import unittest
from pathlib import Path

from tools_test_support import TempDirTestCase, run_tool

import freeze_triage
import symbolize


def report(path: Path, duration: int, ongoing: bool = False) -> Path:
    path.write_text(json.dumps({"schema": symbolize.FREEZE_SCHEMA, "duration_ms": duration, "ongoing": ongoing,
                                "version": "0.0.0-test", "modules": [], "samples": []}), encoding="utf-8")
    return path


def exports_pe(exports: list[tuple[int, str]], forwarder: str) -> bytes:
    """A PE32+ image with one section at RVA 0x1000 (file offset 0x200) holding an export directory:
    `exports` as (RVA, name), plus a forwarder named `forwarder` (its RVA is inside the directory)."""
    image = bytearray(0x800)
    image[0:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, 0x40)
    image[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HH", image, 0x44, 0x8664, 1)
    struct.pack_into("<H", image, 0x54, 240)
    optional = 0x58
    struct.pack_into("<H", image, optional, 0x20B)
    section = optional + 240
    image[section : section + 8] = b".edata\0\0"
    struct.pack_into("<IIII", image, section + 8, 0x600, 0x1000, 0x600, 0x200)
    names = [name for _, name in exports] + [forwarder]
    count = len(names)
    directory = 0x1000
    functions, name_pointers, ordinals = directory + 40, directory + 40 + 4 * count, directory + 40 + 8 * count
    strings = ordinals + 2 * count
    forward_text = b"OTHER.Function\0"
    forward_rva = strings
    strings += len(forward_text)
    rvas = [rva for rva, _ in exports] + [forward_rva]
    size = 0x500

    def at(rva: int) -> int:
        return rva - 0x1000 + 0x200

    struct.pack_into("<IIHHIIIIIII", image, at(directory), 0, 0, 0, 0, 0, 1, count, count, functions,
                     name_pointers, ordinals)
    image[at(forward_rva) : at(forward_rva) + len(forward_text)] = forward_text
    for i, (rva, name) in enumerate(zip(rvas, names)):
        struct.pack_into("<I", image, at(functions) + 4 * i, rva)
        struct.pack_into("<H", image, at(ordinals) + 2 * i, i)
        struct.pack_into("<I", image, at(name_pointers) + 4 * i, strings)
        encoded = name.encode() + b"\0"
        image[at(strings) : at(strings) + len(encoded)] = encoded
        strings += len(encoded)
    struct.pack_into("<II", image, optional + 112, directory, size)  # IMAGE_DIRECTORY_ENTRY_EXPORT
    return bytes(image)


def frame(module: str, offset: int, function: str = "", note: str = "") -> symbolize.Frame:
    return symbolize.Frame(module, offset, [symbolize.Symbol(function, "", 0)] if function else [], note)


class CheckTest(TempDirTestCase):
    def test_check_fails_exactly_on_a_report_over_the_limit(self) -> None:
        report(self.tmp / "freeze-a.json", 250)
        report(self.tmp / "freeze-b.json", 120)
        result = run_tool("freeze_triage.py", "check", str(self.tmp))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("2 freeze reports, 0 over 250 ms", result.stdout)
        report(self.tmp / "freeze-c.json", 251, ongoing=True)
        result = run_tool("freeze_triage.py", "check", str(self.tmp))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("over 250 ms: freeze-c.json 251 ms (ongoing)", result.stdout)
        result = run_tool("freeze_triage.py", "check", "--limit-ms", "300", str(self.tmp))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_a_file_that_is_not_a_freeze_report_is_a_usage_error(self) -> None:
        (self.tmp / "freeze-x.json").write_text(json.dumps({"schema": "something-else"}), encoding="utf-8")
        result = run_tool("freeze_triage.py", "check", str(self.tmp / "freeze-x.json"))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        result = run_tool("freeze_triage.py", "check", str(self.tmp / "missing.json"))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)


class ClassifyTest(unittest.TestCase):
    def test_the_innermost_frostide_function_of_most_samples_is_the_bucket(self) -> None:
        inner = [frame("ntdll.dll", 0x10, "NtReadFile"), frame("frost_app.dll", 0x100, "readReply"),
                 frame("frost_app.dll", 0x200, "setActiveProfile"), frame("Qt6Core.dll", 0x10, "QObject::event")]
        other = [frame("frost_app.dll", 0x300, "paint"), frame("frost_app.dll", 0x200, "setActiveProfile")]
        key, chain = freeze_triage.classify([inner, inner, other])
        self.assertEqual(key, "readReply")
        self.assertEqual(chain, ["readReply", "setActiveProfile"])

    def test_a_stall_below_the_event_loop_names_the_qt_frame(self) -> None:
        sample = [frame("ntdll.dll", 0x10, "ZwWaitForSingleObject+0x14 [export]"),
                  frame("Qt6Gui.dll", 0x20, "QRhi::beginFrame+0xdf [export]"),
                  frame("frost_app.dll", 0x100, "`anonymous namespace'::run"),
                  frame("frost.exe", 0x10, "wmain")]
        key, _ = freeze_triage.classify([sample])
        self.assertEqual(key, "`anonymous namespace'::run > Qt6Gui.dll!QRhi::beginFrame+0xdf [export]")

    def test_unsymbolized_frostide_frames_keep_their_reason(self) -> None:
        sample = [frame("Qt6Core.dll", 0x10, "x"), frame("frost_app.dll", 0x1234, note="no PDB")]
        self.assertEqual(freeze_triage.classify([sample]), ("unsymbolized frost_app.dll+0x1234 (no PDB)", []))
        self.assertEqual(freeze_triage.classify([[frame("Qt6Core.dll", 0x10, "x")]]), ("no FrostIDE frame", []))
        self.assertEqual(freeze_triage.classify([]), ("no samples", []))


class ExportTest(unittest.TestCase):
    def test_named_exports_are_read_sorted_and_forwarders_left_out(self) -> None:
        image = exports_pe([(0x3000, "?exec@QDrag@@QEAA?AW4DropAction@Qt@@V?$QFlags@W4DropAction@Qt@@@@@Z"),
                            (0x2000, "plain_export")], forwarder="Forwarded")
        self.assertEqual(freeze_triage.pe_exports(image),
                         [(0x2000, "plain_export"),
                          (0x3000, "?exec@QDrag@@QEAA?AW4DropAction@Qt@@V?$QFlags@W4DropAction@Qt@@@@@Z")])

    def test_images_without_exports_or_not_pe_give_nothing(self) -> None:
        self.assertEqual(freeze_triage.pe_exports(b"not a PE image at all" * 4), [])
        image = bytearray(exports_pe([(0x2000, "a")], forwarder="f"))
        struct.pack_into("<II", image, 0x58 + 112, 0, 0)
        self.assertEqual(freeze_triage.pe_exports(bytes(image)), [])

    def test_decorated_names_read_as_qualified_names(self) -> None:
        self.assertEqual(freeze_triage.readable_export("?exec@QDrag@@QEAA?AW4DropAction@Qt@@V?$QFlags@@Z"),
                         "QDrag::exec")
        self.assertEqual(freeze_triage.readable_export("?flush@QWidgetRepaintManager@@QEAAXXZ"),
                         "QWidgetRepaintManager::flush")
        self.assertEqual(freeze_triage.readable_export("CreateFileW"), "CreateFileW")
        self.assertEqual(freeze_triage.readable_export("??0QWidget@@QEAA@PEAV0@@Z"), "??0QWidget@@QEAA@PEAV0@@Z")

    @unittest.skipUnless(sys.platform == "win32", "PE images and llvm-readobj's COFF exports are Windows-only here")
    def test_exports_match_llvm_readobj_on_frost_app_dll(self) -> None:
        tree, config = os.environ.get("FROST_TEST_TREE", ""), os.environ.get("FROST_TEST_CONFIG", "")
        image = Path(tree) / "src" / "app" / config / "frost_app.dll" if tree and config else None
        readobj = shutil.which("llvm-readobj")
        if image is None or not image.is_file() or readobj is None:
            self.skipTest(f"no frost_app.dll ({image}) or llvm-readobj on PATH ({readobj})")
        output = subprocess.run([readobj, "--coff-exports", str(image)], capture_output=True, text=True,
                                encoding="utf-8", errors="replace", check=True).stdout
        oracle = sorted((int(rva, 16), name) for name, rva in
                        re.findall(r"Export \{\s*Ordinal: \d+\s*Name: (\S+)\s*RVA: (0x[0-9A-Fa-f]+)", output))
        self.assertTrue(oracle, "llvm-readobj listed no named export")
        self.assertEqual(freeze_triage.pe_exports(image.read_bytes()), oracle)


if __name__ == "__main__":
    unittest.main()
