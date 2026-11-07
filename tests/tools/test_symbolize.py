# SPDX-License-Identifier: Apache-2.0
#
# Tests for tools/symbolize.py (ADR 0022) over synthetic inputs built here: minimal PE32+ and
# ELF64 images (an RSDS debug record, a GNU build-ID note and a chosen first PT_LOAD), crashpad-
# shaped minidumps (system info, module list with CodeView records, exception with an AMD64
# context, thread stacks, crashpad's info stream with annotation objects) and freeze reports.
# llvm-symbolizer is replaced by a fake that answers from a table and records each call, so the
# tests pin what symbolize.py passes (RVAs with --relative-address for PE, file-start-adjusted
# addresses for ELF) and how it reads llvm-symbolizer's JSON array. The command-line contract
# (exit 0 ok, 1 failed, 2 usage) is checked in process and through a child interpreter. Symbol
# archives (FND-13) are checked over a synthetic MSF 7.0 PDB and, on Windows, end to end: clang-cl
# builds a program that writes its own minidump, and only the archive symbolizes its crash frame.

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import unittest
import zipfile
from pathlib import Path
from unittest import mock

from tools_test_support import TempDirTestCase, run_tool

import symbolize

GUID = bytes(range(16))
GUID_TEXT = "03020100-0504-0706-0809-0A0B0C0D0E0F"
BUILD_ID = bytes.fromhex("0123456789abcdef0123456789abcdef01234567")

# Line numbers of the markers in the synthetic sources below.
CRASH_LINE, FREEZE_LINE, CONTRACT_LINE = 4, 9, 3


def pe_bytes(guid: bytes = GUID, age: int = 1, pdb: str = "x.pdb") -> bytes:
    """A PE32+ image: one section at RVA 0x1000 holding the debug directory and its RSDS record
    (which names `pdb`)."""
    image = bytearray(0x400)
    image[0:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, 0x40)
    image[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HH", image, 0x44, 0x8664, 1)  # Machine, NumberOfSections
    struct.pack_into("<H", image, 0x54, 240)  # SizeOfOptionalHeader
    optional = 0x58
    struct.pack_into("<H", image, optional, 0x20B)
    struct.pack_into("<II", image, optional + 112 + 6 * 8, 0x1000, 28)  # IMAGE_DIRECTORY_ENTRY_DEBUG
    section = optional + 240
    image[section : section + 8] = b".rdata\0\0"
    struct.pack_into("<IIII", image, section + 8, 0x200, 0x1000, 0x200, 0x200)
    record = b"RSDS" + guid + struct.pack("<I", age) + pdb.encode() + b"\0"
    struct.pack_into("<IIHHIIII", image, 0x200, 0, 0, 0, 0, 2, len(record), 0x101C, 0x21C)
    image[0x21C : 0x21C + len(record)] = record
    return bytes(image)


def elf_bytes(build_id: bytes = BUILD_ID, first_load: int = 0) -> bytes:
    """An ELF64 image: a PT_LOAD at `first_load` (file offset 0) and a PT_NOTE with the build ID."""
    note = struct.pack("<III", 4, len(build_id), 3) + b"GNU\0" + build_id
    note += b"\0" * (-len(note) % 4)
    header = bytearray(64)
    header[0:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<Q", header, 0x20, 64)  # e_phoff
    struct.pack_into("<HH", header, 0x36, 56, 2)  # e_phentsize, e_phnum
    note_at = 64 + 2 * 56
    load = struct.pack("<IIQQQQQQ", 1, 5, 0, first_load, first_load, 0x10000, 0x10000, 0x1000)
    notes = struct.pack("<IIQQQQQQ", 4, 4, note_at, first_load + note_at, first_load + note_at, len(note), len(note), 4)
    return bytes(header) + load + notes + note


class DumpBuilder:
    """A minidump with `streams` stream directory entries, filled in order by the add_* calls."""

    def __init__(self, streams: int) -> None:
        self.data = bytearray(32 + 12 * streams)
        self.data[0:4] = b"MDMP"
        struct.pack_into("<III", self.data, 4, 0xA793, streams, 32)
        self.next_stream = 0

    def put(self, blob: bytes) -> int:
        self.data += b"\0" * (-len(self.data) % 8)
        rva = len(self.data)
        self.data += blob
        return rva

    def stream(self, stream_type: int, body: bytes) -> None:
        rva = self.put(body)
        struct.pack_into("<III", self.data, 32 + 12 * self.next_stream, stream_type, len(body), rva)
        self.next_stream += 1

    def utf8(self, text: str) -> int:
        raw = text.encode("utf-8")
        return self.put(struct.pack("<I", len(raw)) + raw + b"\0")

    def add_system_info(self, platform: int) -> None:
        body = bytearray(56)
        struct.pack_into("<HHHBB", body, 0, 9, 6, 0, 8, 1)
        struct.pack_into("<I", body, 20, platform)
        self.stream(7, bytes(body))

    def add_modules(self, modules: list[tuple[str, int, int, bytes]]) -> None:
        entries = b""
        for path, base, size, codeview in modules:
            name = path.encode("utf-16-le")
            name_rva = self.put(struct.pack("<I", len(name)) + name + b"\0\0")
            cv_rva = self.put(codeview)
            entry = bytearray(108)
            struct.pack_into("<QI", entry, 0, base, size)
            struct.pack_into("<I", entry, 20, name_rva)
            struct.pack_into("<II", entry, 76, len(codeview), cv_rva)
            entries += bytes(entry)
        self.stream(4, struct.pack("<I", len(modules)) + entries)

    def add_exception(self, thread: int, code: int, address: int, information: list[int], rip: int) -> None:
        context = bytearray(0x4D0)
        struct.pack_into("<Q", context, 0xF8, rip)
        context_rva = self.put(bytes(context))
        body = bytearray(168)
        struct.pack_into("<II", body, 0, thread, 0)
        struct.pack_into("<IIQQI", body, 8, code, 0, 0, address, len(information))
        for k, value in enumerate(information):
            struct.pack_into("<Q", body, 40 + 8 * k, value)
        struct.pack_into("<II", body, 160, len(context), context_rva)
        self.stream(6, bytes(body))

    def add_threads(self, thread: int, stack_start: int, stack: bytes) -> None:
        stack_rva = self.put(stack)
        context_rva = self.put(bytes(0x4D0))
        entry = bytearray(48)
        struct.pack_into("<I", entry, 0, thread)
        struct.pack_into("<QII", entry, 24, stack_start, len(stack), stack_rva)
        struct.pack_into("<II", entry, 40, 0x4D0, context_rva)
        self.stream(3, struct.pack("<I", 1) + bytes(entry))

    def add_crashpad_info(self, annotations: dict[str, str]) -> None:
        objects = b""
        for name, value in annotations.items():
            name_rva = self.utf8(name)
            raw = value.encode("utf-8")
            value_rva = self.put(struct.pack("<I", len(raw)) + raw)
            objects += struct.pack("<IHHI", name_rva, 1, 0, value_rva)
        object_list = struct.pack("<I", len(annotations)) + objects
        objects_rva = self.put(object_list)
        module_info = struct.pack("<I", 1) + struct.pack("<II", 0, 0) * 2 + struct.pack("<II", len(object_list),
                                                                                        objects_rva)
        module_info_rva = self.put(module_info)
        module_list = struct.pack("<I", 1) + struct.pack("<III", 0, len(module_info), module_info_rva)
        module_list_rva = self.put(module_list)
        body = bytearray(52)
        struct.pack_into("<II", body, 44, len(module_list), module_list_rva)
        self.stream(0x43500001, bytes(body))

    def bytes(self) -> bytes:
        assert self.next_stream * 12 + 32 <= len(self.data)
        return bytes(self.data)


def rsds(guid: bytes = GUID, age: int = 1) -> bytes:
    return b"RSDS" + guid + struct.pack("<I", age) + b"x.pdb\0"


def bpel(build_id: bytes = BUILD_ID) -> bytes:
    return struct.pack("<I", 0x4270454C) + build_id


class FakeSymbolizer:
    """llvm-symbolizer's stand-in: answers (object file name, address) from `table` with
    [(function, file, line), ...] (innermost first) and records every command."""

    def __init__(self, table: dict[tuple[str, int], list[tuple[str, str, int]]]) -> None:
        self.table = table
        self.commands: list[list[str]] = []

    # Patched in as Symbolizer.run; an instance attribute lookup does not bind it, so no self.
    def __call__(self, command: list[str]) -> str:
        self.commands.append(command)
        obj = next(arg for arg in command if arg.startswith("--obj="))[len("--obj="):]
        entries = []
        for arg in command:
            if not re.fullmatch(r"0x[0-9a-f]+", arg):
                continue
            symbols = self.table.get((Path(obj).name, int(arg, 16)), [])
            entries.append({"Address": arg, "ModuleName": obj, "Symbol": [
                {"FunctionName": f, "FileName": file, "Line": line, "Column": 0} for f, file, line in symbols
            ] or [{"FunctionName": "??", "FileName": "", "Line": 0}]})
        return json.dumps(entries)


def run_main(argv: list[str], fake: FakeSymbolizer) -> tuple[int, str]:
    out = io.StringIO()
    with mock.patch.object(symbolize.Symbolizer, "run", fake), contextlib.redirect_stdout(out), \
            contextlib.redirect_stderr(io.StringIO()):
        status = symbolize.main(argv)
    return status, out.getvalue()


class IdentityTest(TempDirTestCase):
    def test_pe_rsds_record(self) -> None:
        path = self.tmp / "frost_app.dll"
        path.write_bytes(pe_bytes(age=7))
        image = symbolize.local_image(path)
        self.assertEqual(image.format, "pe")
        self.assertEqual(image.identity, symbolize.Identity("pdb70", guid=GUID_TEXT, age=7))
        self.assertTrue(image.identity.matches(symbolize.codeview_identity(rsds(age=7))))
        self.assertFalse(image.identity.matches(symbolize.codeview_identity(rsds(age=8))))

    def test_elf_build_id_and_file_start(self) -> None:
        path = self.tmp / "frost"
        path.write_bytes(elf_bytes(first_load=0x400000))
        image = symbolize.local_image(path)
        self.assertEqual(image.format, "elf")
        self.assertEqual(image.identity, symbolize.Identity("gnu-build-id", build_id=BUILD_ID.hex()))
        self.assertEqual(image.file_start_vaddr, 0x400000)
        self.assertTrue(image.identity.matches(symbolize.codeview_identity(bpel())))

    def test_truncated_images_have_no_identity(self) -> None:
        for name, data in (("a.dll", pe_bytes()[:0x60]), ("b", elf_bytes()[:80]), ("c", b"junk")):
            path = self.tmp / name
            path.write_bytes(data)
            self.assertEqual(symbolize.local_image(path).identity.kind, "none", name)


class ParsingTest(unittest.TestCase):
    def test_minidump_streams(self) -> None:
        builder = DumpBuilder(5)
        builder.add_system_info(symbolize.PLATFORM_WINDOWS)
        builder.add_modules([("\\\\?\\C:\\b\\frost_app.dll", 0x7FF600000000, 0x10000, rsds())])
        builder.add_exception(42, 0xC0000005, 0x7FF600001234, [1, 0xF00D], 0x7FF600001234)
        builder.add_threads(42, 0x1000, struct.pack("<QQ", 0, 0x7FF600002001))
        builder.add_crashpad_info({"frost.fatal_reason": "contract-violation", "frost.frames": "0x7ff600002001 0x10"})
        dump = symbolize.read_minidump(builder.bytes())
        self.assertEqual(dump.platform, symbolize.PLATFORM_WINDOWS)
        self.assertEqual([(m.path, m.base, m.size) for m in dump.modules],
                         [("\\\\?\\C:\\b\\frost_app.dll", 0x7FF600000000, 0x10000)])
        self.assertEqual(dump.modules[0].identity.guid, GUID_TEXT)
        self.assertEqual((dump.exception_thread, dump.exception_code, dump.exception_information, dump.exception_rip),
                         (42, 0xC0000005, [1, 0xF00D], 0x7FF600001234))
        self.assertEqual(dump.annotations["frost.fatal_reason"], "contract-violation")
        self.assertEqual(symbolize.annotated_frames(dump), [0x7FF600002001, 0x10])
        self.assertEqual(dump.stacks[42][0], 0x1000)

    def test_truncated_minidump_is_input_error(self) -> None:
        builder = DumpBuilder(1)
        builder.add_modules([("C:\\frost_app.dll", 0x1000, 0x1000, rsds())])
        data = builder.bytes()
        with self.assertRaises(symbolize.InputError):
            symbolize.read_minidump(data[:60])
        with self.assertRaises(symbolize.InputError):
            symbolize.read_minidump(b"PK" + data[2:])

    def test_symbolizer_output_shapes(self) -> None:
        array = json.dumps([
            {"Address": "0x10", "Symbol": [{"FunctionName": "inner", "FileName": "a.cpp", "Line": 3},
                                           {"FunctionName": "outer", "FileName": "a.cpp", "Line": 9}]},
            {"Address": "0x20", "Error": {"Message": "no such file or directory"}},
            {"Address": "0x30", "Symbol": [{"FunctionName": "??", "FileName": "", "Line": 0}]},
        ])
        parsed = symbolize.parse_symbolizer_output(array, 4)
        self.assertEqual(parsed[0], [symbolize.Symbol("inner", "a.cpp", 3), symbolize.Symbol("outer", "a.cpp", 9)])
        self.assertEqual(parsed[1], "llvm-symbolizer: no such file or directory")
        self.assertEqual(parsed[2:], ["no symbol", "no llvm-symbolizer output"])
        # A single object, and JSON lines.
        single = json.dumps({"Address": "0x10", "Symbol": [{"FunctionName": "f", "FileName": "b.cpp", "Line": 1}]})
        self.assertEqual(symbolize.parse_symbolizer_output(single, 1), [[symbolize.Symbol("f", "b.cpp", 1)]])
        self.assertEqual(len(symbolize.parse_symbolizer_output(single + "\n" + single + "\n", 2)), 2)

    def test_function_names(self) -> None:
        for name in ("frost::diag::selfTestCrash", "frost::diag::selfTestCrash()",
                     "void __cdecl frost::diag::selfTestCrash(void)"):
            self.assertTrue(symbolize.names_function(name, "frost::diag::selfTestCrash"), name)
        for name in ("`anonymous namespace'::contractEnforceSite", "(anonymous namespace)::contractEnforceSite(int)"):
            self.assertTrue(symbolize.names_function(name, "contractEnforceSite"), name)
        for name in ("frost::diag::selfTestCrashLater", "other::selfTestCrash", "??"):
            self.assertFalse(symbolize.names_function(name, "frost::diag::selfTestCrash"), name)

    def test_source_paths_match_by_suffix(self) -> None:
        self.assertTrue(symbolize.same_source("E:\\dev\\FrostIDE\\src\\diag\\src\\selftest_faults.cpp",
                                              "src/diag/src/selftest_faults.cpp"))
        self.assertTrue(symbolize.same_source("/home/u/frost-src-1a2b/src/diag/src/selftest_faults.cpp",
                                              "src/diag/src/selftest_faults.cpp"))
        self.assertFalse(symbolize.same_source("/x/tests/selftest_faults.cpp", "src/diag/src/selftest_faults.cpp"))


class Fixture:
    """A source root with the three marker files and an artifacts directory, for one platform."""

    CRASH_BASE, CONTRACT_BASE = 0x7FF600000000, 0x7FF700000000
    CRASH_OFFSET, CONTRACT_RETURN_OFFSET, FREEZE_OFFSET = 0x1234, 0x2001, 0x3000

    def __init__(self, root: Path, windows: bool) -> None:
        self.root = root
        self.windows = windows
        self.sources = root / "src-root"
        self.artifacts = root / "artifacts"
        faults = self.sources / "src" / "diag" / "src" / "selftest_faults.cpp"
        faults.parent.mkdir(parents=True)
        lines = ["// faults"] * 12
        lines[CRASH_LINE - 1] = "    *target = 1; // FROST-SELFTEST-CRASH-SITE"
        lines[FREEZE_LINE - 1] = "            spinOnce(counter); // FROST-SELFTEST-FREEZE-SITE"
        faults.write_text("\n".join(lines) + "\n", encoding="utf-8")
        child = self.sources / "tests" / "diag" / "crash_child.cpp"
        child.parent.mkdir(parents=True)
        child.write_text("// child\n[[clang::noinline]] void contractEnforceSite(int value) {\n"
                         "    FROST_PRE(value >= 0); // FROST-SELFTEST-CONTRACT-SITE\n}\n", encoding="utf-8")
        self.faults_file = str(faults) if windows else "/home/u/frost-src-ab/src/diag/src/selftest_faults.cpp"
        self.child_file = str(child)
        images = root / "images"
        images.mkdir()
        self.app = images / ("frost_app.dll" if windows else "frost")
        self.child = images / ("crash_child.exe" if windows else "crash_child")
        self.app.write_bytes(pe_bytes() if windows else elf_bytes())
        self.child.write_bytes(pe_bytes() if windows else elf_bytes(first_load=0x400000))
        self.codeview = rsds() if windows else bpel()

    def shift(self, image: Path) -> int:
        """What symbolize.py must add to a minidump RVA for llvm-symbolizer."""
        return 0 if self.windows or image == self.app else 0x400000

    def table(self, crash_line: int = CRASH_LINE, freeze_line: int = FREEZE_LINE):
        app_crash = "frost::diag::selfTestCrash" if self.windows else "frost::diag::selfTestCrash()"
        freeze = "frost::diag::selfTestFreeze" if self.windows else "frost::diag::selfTestFreeze(std::chrono::duration)"
        site = "`anonymous namespace'::contractEnforceSite" if self.windows else \
            "(anonymous namespace)::contractEnforceSite(int)"
        return {
            (self.app.name, self.CRASH_OFFSET): [(app_crash, self.faults_file, crash_line)],
            (self.child.name, self.CONTRACT_RETURN_OFFSET - 1 + self.shift(self.child)):
                [("frost::core::detail::violated", self.child_file, 1),
                 (site, self.child_file, CONTRACT_LINE)],
            (self.app.name, self.FREEZE_OFFSET - 1): [(freeze, self.faults_file, freeze_line)],
            (self.app.name, self.FREEZE_OFFSET + 0x100): [("frost::diag::`anonymous namespace'::spinOnce",
                                                           self.faults_file, 2)],
        }

    def platform(self) -> int:
        return symbolize.PLATFORM_WINDOWS if self.windows else symbolize.PLATFORM_LINUX

    def write_record(self, kind: str, file: str, status: int, signal: int) -> None:
        directory = self.artifacts / kind
        record = {"schema": symbolize.ARTIFACT_SCHEMA, "kind": kind, "file": file, "status": status,
                  "signal": signal, "marker": symbolize.EXPECTATIONS[kind].marker}
        (directory / "artifact.json").write_text(json.dumps(record), encoding="utf-8")

    def write_crash(self, rip_offset: int = CRASH_OFFSET, address: int = 0xF00D) -> None:
        builder = DumpBuilder(3)
        builder.add_system_info(self.platform())
        builder.add_modules([(str(self.app), self.CRASH_BASE, 0x10000, self.codeview)])
        if self.windows:
            builder.add_exception(1, 0xC0000005, self.CRASH_BASE + rip_offset, [1, address], self.CRASH_BASE + rip_offset)
        else:
            builder.add_exception(1, 11, address, [], self.CRASH_BASE + rip_offset)
        (self.artifacts / "crash").mkdir(parents=True, exist_ok=True)
        (self.artifacts / "crash" / "report.dmp").write_bytes(builder.bytes())
        self.write_record("crash", "report.dmp", 0xC0000005 if self.windows else -1, 0 if self.windows else 11)

    def write_contract(self, codeview: bytes | None = None) -> None:
        builder = DumpBuilder(4)
        builder.add_system_info(self.platform())
        builder.add_modules([(str(self.child), self.CONTRACT_BASE, 0x10000, codeview or self.codeview)])
        code = 0xE0464154 if self.windows else 6
        builder.add_exception(1, code, 0, [], self.CONTRACT_BASE + 0x10)
        frames = [self.CONTRACT_BASE + 0x1500, self.CONTRACT_BASE + self.CONTRACT_RETURN_OFFSET, 0x1234]
        builder.add_crashpad_info({"frost.fatal_reason": "contract-violation",
                                   "frost.frames": " ".join(f"0x{f:x}" for f in frames)})
        (self.artifacts / "contract").mkdir(parents=True, exist_ok=True)
        (self.artifacts / "contract" / "contract.dmp").write_bytes(builder.bytes())
        self.write_record("contract", "contract.dmp", 0xE0464154 if self.windows else -1, 0 if self.windows else 6)

    def write_freeze(self, inside: int = 6, outside: int = 2, duration: int = 300) -> None:
        identity = ({"kind": "pdb70", "guid": GUID_TEXT, "age": 1, "pdb": "x.pdb"} if self.windows else
                    {"kind": "gnu-build-id", "hex": BUILD_ID.hex()})
        samples = []
        for i in range(inside):
            samples.append({"t_ms": 25 + i, "frames": [[0, f"0x{self.FREEZE_OFFSET + 0x100:x}"],
                                                       [0, f"0x{self.FREEZE_OFFSET:x}"], [-1, "0x10"]]})
        for i in range(outside):
            samples.append({"t_ms": 90 + i, "frames": [[-1, "0x99"], [-1, "0x98"], [-1, "0x97"],
                                                       [0, f"0x{self.FREEZE_OFFSET:x}"]]})
        report = {"schema": symbolize.FREEZE_SCHEMA, "duration_ms": duration, "ongoing": False,
                  "thread": {"name": "gui"},
                  "modules": [{"index": 0, "path": str(self.app), "base": "0x7ff600000000", "size": 65536,
                               "id": identity}],
                  "samples": samples}
        (self.artifacts / "freeze").mkdir(parents=True, exist_ok=True)
        (self.artifacts / "freeze" / "freeze-x.json").write_text(json.dumps(report), encoding="utf-8")
        self.write_record("freeze", "freeze-x.json", 0, 0)

    def write_all(self) -> None:
        self.write_crash()
        self.write_contract()
        self.write_freeze()

    def argv(self) -> list[str]:
        return ["check-selftest", "--artifacts", str(self.artifacts), "--llvm-symbolizer", sys.executable,
                "--source-root", str(self.sources)]


class CheckSelfTestTest(TempDirTestCase):
    def check(self, fixture: Fixture, fake: FakeSymbolizer | None = None) -> tuple[int, list[str], FakeSymbolizer]:
        fake = fake or FakeSymbolizer(fixture.table())
        status, out = run_main(fixture.argv(), fake)
        return status, out.splitlines(), fake

    def assert_all_ok(self, lines: list[str]) -> None:
        checks = [line for line in lines if not line.startswith("check-selftest:")]
        self.assertTrue(checks)
        self.assertEqual([line for line in checks if not line.startswith("ok ")], [], "\n".join(lines))

    def test_windows_artifacts_pass(self) -> None:
        fixture = Fixture(self.tmp, windows=True)
        fixture.write_all()
        status, lines, fake = self.check(fixture)
        self.assertEqual(status, 0, "\n".join(lines))
        self.assert_all_ok(lines)
        for kind in ("crash", "contract", "freeze"):
            for name in ("marker", "modules"):
                self.assertTrue(any(line.startswith(f"ok {kind} {name}:") for line in lines), (kind, name, lines))
        self.assertTrue(lines[-1].startswith("check-selftest: ") and " 0 failed" in lines[-1])
        # PE addresses are RVAs, passed with --relative-address.
        self.assertTrue(all("--relative-address" in command for command in fake.commands))
        self.assertTrue(any(f"0x{Fixture.CRASH_OFFSET:x}" in command for command in fake.commands))

    def test_linux_artifacts_pass_with_file_start_adjustment(self) -> None:
        fixture = Fixture(self.tmp, windows=False)
        fixture.write_all()
        status, lines, fake = self.check(fixture)
        self.assertEqual(status, 0, "\n".join(lines))
        self.assert_all_ok(lines)
        self.assertFalse(any("--relative-address" in command for command in fake.commands))
        # crash_child's first PT_LOAD is at 0x400000: a minidump RVA plus that; a freeze
        # report's offsets (from the load bias) unchanged.
        child_calls = [c for c in fake.commands if any(a.endswith("crash_child") for a in c)]
        self.assertIn(f"0x{0x400000 + Fixture.CONTRACT_RETURN_OFFSET - 1:x}", child_calls[0])
        self.assertTrue(any(f"0x{Fixture.FREEZE_OFFSET - 1:x}" in command for command in fake.commands))

    def test_wrong_line_wrong_address_and_identity_mismatch_fail(self) -> None:
        fixture = Fixture(self.tmp, windows=True)
        fixture.write_crash(address=0xBEEF)
        fixture.write_contract(codeview=rsds(age=2))
        fixture.write_freeze()
        status, lines, _ = self.check(fixture, FakeSymbolizer(fixture.table(crash_line=CRASH_LINE + 1)))
        self.assertEqual(status, 1)
        failed = {" ".join(line.split()[:3]) for line in lines if line.startswith("FAILED")}
        self.assertEqual(failed, {"FAILED crash fault:", "FAILED crash rip:", "FAILED contract modules:",
                                  "FAILED contract frames:"}, "\n".join(lines))
        self.assertTrue(any("identity mismatch" in line for line in lines))

    def test_rip_outside_every_module_fails(self) -> None:
        fixture = Fixture(self.tmp, windows=False)
        fixture.write_all()
        fixture.write_crash(rip_offset=0x20000)
        status, lines, _ = self.check(fixture)
        self.assertEqual(status, 1)
        self.assertTrue(any(line.startswith("FAILED crash rip:") and "lies in no module" in line for line in lines))

    def test_freeze_needs_half_the_samples_and_enough_of_them(self) -> None:
        fixture = Fixture(self.tmp, windows=True)
        fixture.write_all()
        fixture.write_freeze(inside=3, outside=4)
        status, lines, _ = self.check(fixture)
        self.assertEqual(status, 1)
        self.assertTrue(any(line.startswith("FAILED freeze samples: 3 of 7") for line in lines), lines)
        fixture.write_freeze(inside=4, outside=4)  # Exactly half passes.
        self.assertEqual(self.check(fixture)[0], 0)
        fixture.write_freeze(inside=4, outside=0, duration=300)
        status, lines, _ = self.check(fixture)
        self.assertTrue(any(line.startswith("FAILED freeze report: 300 ms, 4 samples") for line in lines), lines)
        fixture.write_freeze(inside=6, outside=0, duration=249)
        self.assertEqual(self.check(fixture)[0], 1)
        # Within three lines of the marker counts; four lines off does not.
        fixture.write_freeze()
        self.assertEqual(self.check(fixture, FakeSymbolizer(fixture.table(freeze_line=FREEZE_LINE - 3)))[0], 0)
        self.assertEqual(self.check(fixture, FakeSymbolizer(fixture.table(freeze_line=FREEZE_LINE + 4)))[0], 1)

    def test_a_missing_kind_or_a_bad_record_fails(self) -> None:
        fixture = Fixture(self.tmp, windows=True)
        fixture.write_crash()
        fixture.write_freeze()
        (fixture.artifacts / "contract").mkdir()
        (fixture.artifacts / "contract" / "artifact.json").write_text("{\"schema\": \"other\"}", encoding="utf-8")
        status, lines, _ = self.check(fixture)
        self.assertEqual(status, 1)
        self.assertTrue(any(line.startswith("FAILED contract artifact:") for line in lines), lines)
        (fixture.artifacts / "contract" / "artifact.json").unlink()
        status, lines, _ = self.check(fixture)
        self.assertTrue(any(line.startswith("FAILED contract artifact: no ") for line in lines), lines)

    def test_a_truncated_dump_fails_its_kind_only(self) -> None:
        fixture = Fixture(self.tmp, windows=True)
        fixture.write_all()
        dump = fixture.artifacts / "crash" / "report.dmp"
        dump.write_bytes(dump.read_bytes()[:100])
        status, lines, _ = self.check(fixture)
        self.assertEqual(status, 1)
        self.assertTrue(any(line.startswith("FAILED crash artifact: truncated minidump") for line in lines), lines)
        self.assertTrue(any(line.startswith("ok freeze samples:") for line in lines), lines)

    def test_a_missing_marker_fails(self) -> None:
        fixture = Fixture(self.tmp, windows=True)
        fixture.write_all()
        (fixture.sources / "tests" / "diag" / "crash_child.cpp").write_text("// no marker\n", encoding="utf-8")
        status, lines, _ = self.check(fixture)
        self.assertEqual(status, 1)
        self.assertTrue(any(line.startswith("FAILED contract marker:") for line in lines), lines)


class CommandsTest(TempDirTestCase):
    def test_minidump_command_prints_the_crash_site_and_frames(self) -> None:
        fixture = Fixture(self.tmp, windows=True)
        fixture.write_crash()
        fixture.write_contract()
        status, out = run_main(["minidump", str(fixture.artifacts / "crash" / "report.dmp"), "--llvm-symbolizer",
                                sys.executable], FakeSymbolizer(fixture.table()))
        self.assertEqual(status, 0, out)
        self.assertIn(f"crash  frost_app.dll+0x{Fixture.CRASH_OFFSET:x}  frost::diag::selfTestCrash  "
                      f"selftest_faults.cpp:{CRASH_LINE}", out)
        status, out = run_main(["minidump", str(fixture.artifacts / "contract" / "contract.dmp"), "--llvm-symbolizer",
                                sys.executable], FakeSymbolizer(fixture.table()))
        self.assertIn("frost.fatal_reason: contract-violation", out)
        self.assertRegex(out, r"frame  1 crash_child\.exe\+0x2000  frost::core::detail::violated  crash_child\.cpp:1 "
                              r"\(inlined into `anonymous namespace'::contractEnforceSite\)")

    def test_freeze_command_maps_each_address_to_its_own_frame(self) -> None:
        fixture = Fixture(self.tmp, windows=True)
        fixture.write_freeze(inside=2, outside=0)
        status, out = run_main(["freeze", str(fixture.artifacts / "freeze" / "freeze-x.json"), "--samples",
                                "--llvm-symbolizer", sys.executable], FakeSymbolizer(fixture.table()))
        self.assertEqual(status, 1, "the -1 frames stay unsymbolized")
        self.assertIn("freeze 300 ms on thread gui, 2 samples", out)
        self.assertIn("2 x frost::diag::`anonymous namespace'::spinOnce", out)
        self.assertIn(f"   1 frost_app.dll+0x{Fixture.FREEZE_OFFSET - 1:x}  frost::diag::selfTestFreeze  "
                      f"selftest_faults.cpp:{FREEZE_LINE}", out)


class CommandLineTest(TempDirTestCase):
    def test_usage_errors_exit_2(self) -> None:
        self.assertEqual(run_tool("symbolize.py").returncode, 2)
        self.assertEqual(run_tool("symbolize.py", "check-selftest", "--llvm-symbolizer", sys.executable).returncode, 2)
        missing = run_tool("symbolize.py", "check-selftest", "--artifacts", str(self.tmp / "none"),
                           "--llvm-symbolizer", sys.executable)
        self.assertEqual(missing.returncode, 2)
        self.assertIn("no artifacts directory", missing.stderr)
        no_tool = run_tool("symbolize.py", "check-selftest", "--artifacts", str(self.tmp), "--llvm-symbolizer",
                           str(self.tmp / "no-llvm-symbolizer"))
        self.assertEqual(no_tool.returncode, 2)
        self.assertIn("llvm-symbolizer not found", no_tool.stderr)
        unreadable = run_tool("symbolize.py", "minidump", str(self.tmp / "none.dmp"), "--llvm-symbolizer",
                              sys.executable)
        self.assertEqual(unreadable.returncode, 2)

    def test_an_empty_artifacts_directory_fails_every_kind(self) -> None:
        result = run_tool("symbolize.py", "check-selftest", "--artifacts", str(self.tmp), "--llvm-symbolizer",
                          sys.executable)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual([line.split(":")[0] for line in result.stdout.splitlines()[:3]],
                         ["FAILED crash artifact", "FAILED contract artifact", "FAILED freeze artifact"])


def msf_bytes(guid: bytes = GUID, info_age: int = 1, dbi_age: int | None = 1) -> bytes:
    """A minimal MSF 7.0 file (a PDB) with 512-byte blocks: the superblock, a free-block map, the
    block map, the stream directory, the PDB info stream (stream 1) and, when `dbi_age` is given, a
    DBI stream (stream 3) whose age field is `dbi_age`."""
    block = 512
    info = struct.pack("<III", 20000404, 0x12345678, info_age) + guid
    dbi = struct.pack("<iII", -1, 19990903, dbi_age) if dbi_age is not None else b""
    sizes = [0, len(info), 0, len(dbi)] if dbi_age is not None else [0, len(info)]
    lists = [[], [4], [], [5]] if dbi_age is not None else [[], [4]]
    directory = struct.pack("<I", len(sizes)) + struct.pack(f"<{len(sizes)}I", *sizes)
    for blocks in lists:
        directory += struct.pack(f"<{len(blocks)}I", *blocks)
    superblock = symbolize.MSF_MAGIC + struct.pack("<IIIIII", block, 1, 6, len(directory), 0, 2)
    pages = [superblock, b"", struct.pack("<I", 3), directory, info, dbi]
    return b"".join(page.ljust(block, b"\0") for page in pages)


class SymbolArchiveTest(TempDirTestCase):
    """The symbol archive tools/stage.ps1 writes (FND-13): images with the PDBs that match them."""

    def stage(self, pdb: bytes | None) -> tuple[Path, Path]:
        stage, pdbs = self.tmp / "stage", self.tmp / "pdbs"
        stage.mkdir()
        pdbs.mkdir()
        (stage / "frost.exe").write_bytes(pe_bytes())  # Its RSDS record names x.pdb.
        (stage / "notes.txt").write_text("not an image")
        if pdb is not None:
            (pdbs / "x.pdb").write_bytes(pdb)
        return stage, pdbs

    def archive(self, stage: Path, pdbs: Path, *required: str) -> tuple[int, str, Path]:
        output = self.tmp / "stage-symbols.zip"
        argv = ["archive", "--stage", str(stage), "--pdb-dir", str(pdbs), "--output", str(output)]
        for name in required:
            argv += ["--require", name]
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            status = symbolize.main(argv)
        return status, out.getvalue(), output

    def test_pdb_identity_reads_the_info_guid_and_the_dbi_age(self) -> None:
        self.assertEqual(symbolize.pdb_identity(msf_bytes(info_age=1, dbi_age=3)),
                         symbolize.Identity("pdb70", guid=GUID_TEXT, age=3))
        self.assertEqual(symbolize.pdb_identity(msf_bytes(info_age=2, dbi_age=None)).age, 2)
        with self.assertRaises(symbolize.InputError):
            symbolize.pdb_identity(b"not a pdb")
        with self.assertRaises(symbolize.InputError):
            symbolize.pdb_identity(msf_bytes()[:700])

    def test_archives_each_image_with_its_matching_pdb_and_a_manifest(self) -> None:
        stage, pdbs = self.stage(msf_bytes())
        status, out, output = self.archive(stage, pdbs, "frost.exe")
        self.assertEqual(status, 0, out)
        self.assertIn(f"module frost.exe + x.pdb (pdb70 {GUID_TEXT}/1)", out)
        with zipfile.ZipFile(output) as archive:
            self.assertEqual(sorted(archive.namelist()), ["frost.exe", "symbols.json", "x.pdb"])
            manifest = json.loads(archive.read("symbols.json"))
        self.assertEqual(manifest["schema"], "frost-symbol-archive/1")
        self.assertEqual(manifest["modules"][0]["identity"], f"pdb70 {GUID_TEXT}/1")
        self.assertEqual(manifest["modules"][0]["pdbSha256"], hashlib.sha256(msf_bytes()).hexdigest())
        unpacked = self.tmp / "unpacked"
        unpacked.mkdir()
        symbolize.unpack_symbol_archive(output, unpacked)
        self.assertEqual((unpacked / "x.pdb").read_bytes(), msf_bytes())
        self.assertEqual(symbolize.local_image(unpacked / "frost.exe").identity.guid, GUID_TEXT)

    def test_a_stale_or_missing_pdb_fails_a_required_image_and_writes_nothing(self) -> None:
        stage, pdbs = self.stage(msf_bytes(dbi_age=2))
        status, out, output = self.archive(stage, pdbs, "frost.exe")
        self.assertEqual(status, 1)
        self.assertIn("FAIL   frost.exe:", out)
        self.assertIn(f"the image wants pdb70 {GUID_TEXT}/1", out)
        self.assertFalse(output.exists())
        (pdbs / "x.pdb").unlink()
        status, out, _ = self.archive(stage, pdbs, "frost.exe")
        self.assertEqual(status, 1)
        self.assertIn("x.pdb not found", out)
        status, out, _ = self.archive(stage, pdbs, "frost_app.dll")
        self.assertEqual(status, 1)
        self.assertIn("frost_app.dll: not in the stage", out)
        status, out, output = self.archive(stage, pdbs)
        self.assertEqual(status, 0, "an image nobody requires is skipped")
        self.assertIn("skip   frost.exe: x.pdb not found", out)

    def test_check_only_runs_the_identity_checks_and_writes_nothing(self) -> None:
        # stage.ps1's preflight before it creates the destination (R0-close item 7).
        def check(stage: Path, pdbs: Path, *required: str) -> tuple[int, str]:
            argv = ["archive", "--stage", str(stage), "--pdb-dir", str(pdbs), "--check"]
            for name in required:
                argv += ["--require", name]
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                status = symbolize.main(argv)
            return status, out.getvalue()

        stage, pdbs = self.stage(msf_bytes())
        status, out = check(stage, pdbs, "frost.exe")
        self.assertEqual(status, 0, out)
        self.assertIn("check: 1 modules match their PDBs", out)
        (pdbs / "x.pdb").write_bytes(msf_bytes(dbi_age=2))
        status, out = check(stage, pdbs, "frost.exe")
        self.assertEqual(status, 1)
        self.assertIn(f"the image wants pdb70 {GUID_TEXT}/1", out)
        self.assertEqual(sorted(p.name for p in self.tmp.iterdir()), ["pdbs", "stage"], "nothing written")
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(symbolize.main(["archive", "--stage", str(stage), "--pdb-dir", str(pdbs)]), 2,
                             "--output or --check is required")

    def test_unpacking_rejects_tampered_members_and_paths(self) -> None:
        stage, pdbs = self.stage(msf_bytes())
        status, out, output = self.archive(stage, pdbs, "frost.exe")
        self.assertEqual(status, 0, out)
        with zipfile.ZipFile(output) as archive:
            manifest = json.loads(archive.read("symbols.json"))
        for name, member, content in (("digest", "x.pdb", b"other"), ("path", "../x.pdb", msf_bytes())):
            tampered = self.tmp / f"{name}.zip"
            changed = json.loads(json.dumps(manifest))
            changed["modules"][0]["pdb"] = member
            with zipfile.ZipFile(tampered, "w") as archive:
                archive.writestr("symbols.json", json.dumps(changed))
                archive.writestr("frost.exe", pe_bytes())
                archive.writestr(member, content)
            destination = self.tmp / f"{name}-out"
            destination.mkdir()
            with self.assertRaises(symbolize.InputError, msg=name):
                symbolize.unpack_symbol_archive(tampered, destination)
        self.assertFalse((self.tmp / "x.pdb").exists())

    def test_symbol_archive_is_searched_like_a_search_dir(self) -> None:
        stage, pdbs = self.stage(msf_bytes())
        _, _, output = self.archive(stage, pdbs, "frost.exe")
        seen: list[list[Path]] = []

        class Recording(symbolize.Symbolizer):
            def __init__(self, executable: str, search_dirs: list[Path]) -> None:
                super().__init__(executable, search_dirs)
                seen.append(list(search_dirs))
                if not (search_dirs[-1] / "frost.exe").is_file() or not (search_dirs[-1] / "x.pdb").is_file():
                    raise AssertionError("the archive is unpacked before symbolizing")

        with mock.patch.object(symbolize, "Symbolizer", Recording), contextlib.redirect_stderr(io.StringIO()):
            status = symbolize.main(["minidump", str(self.tmp / "none.dmp"), "--llvm-symbolizer", sys.executable,
                                     "--search-dir", str(self.tmp), "--symbol-archive", str(output)])
        self.assertEqual(status, 2, "the dump is unreadable")
        self.assertEqual(len(seen), 1)
        self.assertEqual(seen[0][0], self.tmp)
        self.assertFalse(seen[0][1].exists(), "the unpacked archive is removed afterwards")


# A program that faults at a marked line and writes its own minidump (MiniDumpWriteDump from an
# SEH filter, so no crash dialog or WER report), for the real-toolchain archive test.
ARCHIVE_PROBE = r"""// Symbol-archive probe for tests/tools/test_symbolize.py.
#include <windows.h>
#include <dbghelp.h>

static int writeDump(const wchar_t* path, EXCEPTION_POINTERS* info) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception = {GetCurrentThreadId(), info, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, &exception, NULL, NULL);
        CloseHandle(file);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

__declspec(noinline) void archiveCrashSite(volatile int* target) {
    *target = 7; // FROST-ARCHIVE-CRASH-SITE
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2)
        return 2;
    __try {
        archiveCrashSite((volatile int*)(ULONG_PTR)0xf00d);
    } __except (writeDump(argv[1], GetExceptionInformation())) {
        return 3;
    }
    return 0;
}
"""


def llvm_tool(name: str) -> str | None:
    """`name` from $FROST_LLVM_ROOT/bin, else PATH (the pinned LLVM that tools/env.ps1 puts there)."""
    root = os.environ.get("FROST_LLVM_ROOT")
    if root and (Path(root) / "bin" / f"{name}.exe").is_file():
        return str(Path(root) / "bin" / f"{name}.exe")
    return shutil.which(name)


@unittest.skipUnless(os.name == "nt", "PE images and PDBs: the Windows leg")
class RealSymbolArchiveTest(TempDirTestCase):
    """FND-13 end to end with the real toolchain: clang-cl and lld-link build a program and its PDB,
    `archive` packs them, the program writes a real minidump, the build and the stage are deleted,
    and only --symbol-archive lets llvm-symbolizer name the faulting function and line."""

    def test_a_minidump_frame_symbolizes_from_the_archive_alone(self) -> None:
        clang_cl, llvm_symbolizer = llvm_tool("clang-cl"), llvm_tool("llvm-symbolizer")
        if clang_cl is None or llvm_symbolizer is None:
            self.fail("clang-cl and llvm-symbolizer of the pinned LLVM are required (tools/env.ps1)")
        build, stage = self.tmp / "build", self.tmp / "stage"
        build.mkdir()
        stage.mkdir()
        source = build / "probe.c"
        source.write_text(ARCHIVE_PROBE, encoding="utf-8")
        line = next(i for i, text in enumerate(ARCHIVE_PROBE.splitlines(), 1) if "FROST-ARCHIVE-CRASH-SITE" in text)
        compiled = subprocess.run([clang_cl, "/nologo", "/Z7", "/Od", str(source), f"/Fo{build}\\",
                                   f"/Fe{build / 'probe.exe'}", "/link", "/DEBUG", "/INCREMENTAL:NO", "dbghelp.lib"],
                                  capture_output=True, text=True, cwd=build, timeout=120, check=False)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        self.assertTrue((build / "probe.pdb").is_file())
        shutil.copy2(build / "probe.exe", stage / "probe.exe")
        archive = self.tmp / "stage-symbols.zip"
        packed = run_tool("symbolize.py", "archive", "--stage", str(stage), "--pdb-dir", str(build), "--output",
                          str(archive), "--require", "probe.exe")
        self.assertEqual(packed.returncode, 0, packed.stdout + packed.stderr)
        dump = self.tmp / "probe.dmp"
        ran = subprocess.run([str(stage / "probe.exe"), str(dump)], capture_output=True, timeout=60, check=False)
        self.assertEqual(ran.returncode, 3, "the probe faults and writes its dump")
        self.assertTrue(dump.is_file())
        # Nothing but the archive is left: neither the recorded image path nor the linker's PDB path.
        shutil.rmtree(build)
        shutil.rmtree(stage)
        without = run_tool("symbolize.py", "minidump", str(dump), "--llvm-symbolizer", llvm_symbolizer)
        self.assertEqual(without.returncode, 1, without.stdout + without.stderr)
        result = run_tool("symbolize.py", "minidump", str(dump), "--llvm-symbolizer", llvm_symbolizer,
                          "--symbol-archive", str(archive))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertRegex(result.stdout, rf"crash  probe\.exe\+0x[0-9a-f]+  archiveCrashSite  probe\.c:{line}\b")


if __name__ == "__main__":
    unittest.main()
