#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# symbolize.py: turns FrostIDE's crash dumps and freeze reports into function names and source
# lines, and checks the self-test artifacts verify -Full collects (ADR 0022).
#
#   python -I tools/symbolize.py minidump <file.dmp> [common options]
#   python -I tools/symbolize.py freeze <freeze-*.json> [--samples] [common options]
#   python -I tools/symbolize.py check-selftest --artifacts <dir> [--source-root <repo>] [common options]
#   python -I tools/symbolize.py archive --stage <dir> --pdb-dir <dir> --output <zip> [--require <image>]
#
# Common options: --llvm-symbolizer <path> (default: $FROST_LLVM_ROOT/bin, then PATH),
# --search-dir <dir> (repeatable; where to look for a module whose recorded path is gone) and
# --symbol-archive <zip> (repeatable; a stage's symbol archive, see "Symbol archives" below).
#
# Every module is checked against the identity the dump or report recorded (the PDB GUID and
# age of a PE image's RSDS record, the GNU build ID of an ELF image's NT_GNU_BUILD_ID note)
# before its addresses are symbolized; a mismatching or missing module leaves its frames
# unsymbolized, and check-selftest fails on it. Symbolization is llvm-symbolizer's
# (--output-style=JSON, which prints one JSON array for all the addresses of a call):
#   PE    addresses are RVAs (address - module base), passed with --relative-address;
#   ELF   a minidump's module base is the mapping of the file's start, so addresses are the RVA
#         plus the first PT_LOAD's p_vaddr - p_offset (0 for position-independent images); a
#         freeze report's offsets are already from the load bias, so they are ELF virtual
#         addresses as they stand (frost/diag/freeze_report.hpp).
# Frames after the first of a stack are return addresses and are looked up one byte earlier,
# inside the call; frost.frames (a contract violation's stack) holds return addresses only.
#
# check-selftest reads <dir>/<kind>/artifact.json for each kind the frost_process_tests
# self-tests write (crash, contract, freeze), whose "file" names the dump or report beside it,
# and prints one line per check:
#   crash     the status is an access violation (Windows 0xC0000005, Linux SIGSEGV); the fault
#             address is 0xf00d (Windows ExceptionInformation[1] of a write, Linux si_addr as
#             crashpad records it in ExceptionAddress); the exception thread's context RIP lies
#             in a verified module and symbolizes to frost::diag::selfTestCrash on the line
#             marked FROST-SELFTEST-CRASH-SITE in src/diag/src/selftest_faults.cpp.
#   contract  the status is frost's fatal exception (Windows 0xE0464154, Linux SIGABRT);
#             frost.fatal_reason is contract-violation; among the first 12 frames of the
#             frost.frames annotation, in verified modules, one symbolizes to crash_child's
#             contractEnforceSite on the line marked FROST-SELFTEST-CONTRACT-SITE (the
#             FROST_PRE) in tests/diag/crash_child.cpp.
#   freeze    the report is final, at least 250 ms long with at least 5 samples; in at least
#             half of the samples one of the top 3 frames, in verified modules, symbolizes to
#             frost::diag::selfTestFreeze within 3 lines of FROST-SELFTEST-FREEZE-SITE.
# A kind without an artifact fails. Modules without a file by design (the Linux vDSO) are not
# verified; their frames stay unsymbolized.
#
# Layouts: Microsoft's public minidump documentation (minidumpapiset.h) and PE format
# documentation (the debug directory, IMAGE_DEBUG_TYPE_CODEVIEW), crashpad's
# minidump_extensions.h for its info stream, annotations and ELF CodeView record ('BpEL';
# Apache-2.0), and the ELF gABI with the GNU build-ID note. Output: text on stdout. Exit status
# 0 when everything symbolized (or every check passed), 1 when something did not (or a check
# failed), 2 on usage errors, a missing artifacts directory or llvm-symbolizer, or (minidump,
# freeze) unreadable input. Python 3.11+, standard library only; run with -I.

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ARTIFACT_SCHEMA = "frost-selftest-artifact/1"
FREEZE_SCHEMA = "frost-freeze-report/1"

SELFTEST_CRASH_ADDRESS = 0xF00D
EXCEPTION_ACCESS_VIOLATION = 0xC0000005
FATAL_EXCEPTION_CODE = 0xE0464154  # frost::diag::kFatalExceptionCode
LINUX_SIGABRT = 6
LINUX_SIGSEGV = 11
PLATFORM_WINDOWS = 2  # VER_PLATFORM_WIN32_NT
PLATFORM_LINUX = 0x8201  # crashpad's MinidumpOS kMinidumpOSLinux

CONTRACT_FRAMES = 12  # How many frost.frames entries the contract check examines.
FREEZE_TOP_FRAMES = 3
FREEZE_LINE_SLACK = 3
FREEZE_MIN_SAMPLES = 5
FREEZE_MIN_DURATION_MS = 250


@dataclass(frozen=True)
class Expectation:
    """What a self-test artifact must symbolize to: `function` at the line of `source` (relative
    to the source root, '/'-separated) that holds `marker`."""

    function: str
    marker: str
    source: str


EXPECTATIONS = {
    "crash": Expectation("frost::diag::selfTestCrash", "FROST-SELFTEST-CRASH-SITE", "src/diag/src/selftest_faults.cpp"),
    "contract": Expectation("contractEnforceSite", "FROST-SELFTEST-CONTRACT-SITE", "tests/diag/crash_child.cpp"),
    "freeze": Expectation("frost::diag::selfTestFreeze", "FROST-SELFTEST-FREEZE-SITE",
                          "src/diag/src/selftest_faults.cpp"),
}


class InputError(Exception):
    """Unreadable or malformed input."""


class UsageError(Exception):
    """A bad command line or a missing path the command line named (exit status 2)."""


# --- Module identity -------------------------------------------------------------------------


@dataclass(frozen=True)
class Identity:
    kind: str  # "pdb70", "gnu-build-id" or "none"
    guid: str = ""  # Upper-case, dashed, for pdb70.
    age: int = 0
    build_id: str = ""  # Lower-case hex, for gnu-build-id.

    def matches(self, other: Identity) -> bool:
        if self.kind != other.kind or self.kind == "none":
            return False
        if self.kind == "pdb70":
            return self.guid.upper() == other.guid.upper() and self.age == other.age
        return self.build_id.lower() == other.build_id.lower()

    def text(self) -> str:
        if self.kind == "pdb70":
            return f"pdb70 {self.guid}/{self.age}"
        if self.kind == "gnu-build-id":
            return f"build-id {self.build_id}"
        return "no identity"


def format_guid(raw: bytes) -> str:
    """A GUID's 16 bytes (little-endian Data1..Data3) as XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX."""
    data1, data2, data3 = struct.unpack_from("<IHH", raw, 0)
    tail = raw[8:16].hex().upper()
    return f"{data1:08X}-{data2:04X}-{data3:04X}-{tail[:4]}-{tail[4:]}"


def codeview_identity(record: bytes) -> Identity:
    """The identity in a CodeView record: 'RSDS' (PDB 7.0) or crashpad's 'BpEL' (ELF build ID)."""
    if len(record) >= 24 and record[:4] == b"RSDS":
        return Identity("pdb70", guid=format_guid(record[4:20]), age=struct.unpack_from("<I", record, 20)[0])
    if len(record) > 4 and struct.unpack_from("<I", record, 0)[0] == 0x4270454C:
        return Identity("gnu-build-id", build_id=record[4:].hex())
    return Identity("none")


@dataclass(frozen=True)
class LocalImage:
    """A module's file on this machine."""

    path: Path
    format: str  # "pe" or "elf"
    identity: Identity
    file_start_vaddr: int = 0  # ELF: the virtual address of file offset 0 (first PT_LOAD).


def pe_image(path: Path, data: bytes) -> LocalImage:
    """A PE32/PE32+ image and the RSDS record of its debug directory."""
    record = pe_codeview_record(data)
    return LocalImage(path, "pe", codeview_identity(record) if record is not None else Identity("none"))


def pe_codeview_record(data: bytes) -> bytes | None:
    """The CodeView record (IMAGE_DEBUG_TYPE_CODEVIEW) of a PE32/PE32+ image's debug directory."""
    try:
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe : pe + 4] != b"PE\0\0":
            return None
        sections, optional_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
        optional = pe + 24
        magic = struct.unpack_from("<H", data, optional)[0]
        directories = optional + (112 if magic == 0x20B else 96)
        debug_rva, debug_size = struct.unpack_from("<II", data, directories + 6 * 8)
        table = optional + optional_size

        def file_offset(rva: int) -> int | None:
            for i in range(sections):
                virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from("<IIII", data,
                                                                                          table + i * 40 + 8)
                if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
                    return rva - virtual_address + raw_pointer
            return None

        start = file_offset(debug_rva) if debug_size else None
        if start is not None:
            for entry in range(start, start + debug_size, 28):
                kind, size, _, pointer = struct.unpack_from("<IIII", data, entry + 12)
                if kind == 2:  # IMAGE_DEBUG_TYPE_CODEVIEW
                    return data[pointer : pointer + size]
    except struct.error:
        pass
    return None


def rsds_pdb_name(record: bytes) -> str:
    """The file name of the PDB an RSDS record names (its path is the linker's), or ""."""
    if len(record) <= 24 or record[:4] != b"RSDS":
        return ""
    recorded = record[24:].split(b"\0", 1)[0].decode("utf-8", errors="replace")
    return base_name(recorded)


def elf_image(path: Path, data: bytes) -> LocalImage:
    """A little-endian ELF64 image: its NT_GNU_BUILD_ID note (PT_NOTE segments) and the virtual
    address of its file start (the first PT_LOAD's p_vaddr - p_offset)."""
    if data[4:6] != b"\x02\x01":
        return LocalImage(path, "elf", Identity("none"))
    identity = Identity("none")
    file_start: int | None = None
    try:
        phoff = struct.unpack_from("<Q", data, 0x20)[0]
        phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
        for i in range(phnum):
            header = phoff + i * phentsize
            p_type = struct.unpack_from("<I", data, header)[0]
            p_offset, p_vaddr = struct.unpack_from("<QQ", data, header + 8)
            p_filesz = struct.unpack_from("<Q", data, header + 32)[0]
            if p_type == 1 and file_start is None:  # PT_LOAD; the gABI orders them by p_vaddr.
                file_start = p_vaddr - p_offset
            if p_type != 4 or identity.kind != "none":  # PT_NOTE
                continue
            at, end = p_offset, p_offset + p_filesz
            while at + 12 <= end:
                namesz, descsz, note_type = struct.unpack_from("<III", data, at)
                name_at = at + 12
                desc_at = name_at + ((namesz + 3) & ~3)
                if note_type == 3 and data[name_at : name_at + namesz] == b"GNU\0":  # NT_GNU_BUILD_ID
                    identity = Identity("gnu-build-id", build_id=data[desc_at : desc_at + descsz].hex())
                    break
                at = desc_at + ((descsz + 3) & ~3)
    except struct.error:
        pass
    return LocalImage(path, "elf", identity, file_start or 0)


def local_image(path: Path) -> LocalImage | None:
    try:
        data = path.read_bytes()
    except OSError:
        return None
    if data[:2] == b"MZ":
        return pe_image(path, data)
    if data[:4] == b"\x7fELF":
        return elf_image(path, data)
    return LocalImage(path, "unknown", Identity("none"))


# --- Symbol archives (FND-13) ----------------------------------------------------------------
#
# tools/stage.ps1 writes <stage>-symbols.zip next to a staged folder: each FrostIDE PE image of
# the stage together with the PDB its RSDS record names, both flat at the archive root, plus
# symbols.json, a manifest {"schema": "frost-symbol-archive/1", "modules": [{"image", "pdb",
# "identity", "imageSha256", "pdbSha256"}]}. A PDB is archived only when its own GUID and age match
# the image's RSDS record, so an archive never pairs an image with a stale PDB. The PDB's identity
# comes from its MSF 7.0 container (LLVM's "The PDB File Format" documentation): the superblock,
# the stream directory, the PDB info stream (stream 1: version, signature, age, GUID) and the DBI
# stream's age (stream 3, offset 8), which is the age the linker writes into the image.
#
# --symbol-archive <zip> (any symbolizing command) unpacks an archive into a temporary directory
# and searches it like --search-dir; llvm-symbolizer then finds each PDB beside its image when the
# path the linker recorded is gone.

SYMBOL_ARCHIVE_SCHEMA = "frost-symbol-archive/1"
SYMBOL_ARCHIVE_MANIFEST = "symbols.json"
MSF_MAGIC = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0"


def msf_streams(data: bytes) -> list[bytes]:
    """The streams of an MSF 7.0 file (a PDB); InputError when it is not one or is truncated."""
    if data[: len(MSF_MAGIC)] != MSF_MAGIC:
        raise InputError("not an MSF 7.0 (PDB) file")
    try:
        block_size, _, block_count, directory_bytes, _, map_block = struct.unpack_from("<IIIIII", data,
                                                                                       len(MSF_MAGIC))
        if block_size not in (512, 1024, 2048, 4096) or block_count * block_size > len(data):
            raise InputError("bad MSF superblock")

        def blocks(indexes: list[int], size: int) -> bytes:
            out = b"".join(data[i * block_size : (i + 1) * block_size] for i in indexes)
            if len(out) < size or any(i >= block_count for i in indexes):
                raise InputError("MSF stream outside the file")
            return out[:size]

        directory_blocks = (directory_bytes + block_size - 1) // block_size
        directory = blocks(list(struct.unpack_from(f"<{directory_blocks}I", data, map_block * block_size)),
                           directory_bytes)
        count = struct.unpack_from("<I", directory, 0)[0]
        sizes = [0 if s == 0xFFFFFFFF else s for s in struct.unpack_from(f"<{count}I", directory, 4)]
        at = 4 + 4 * count
        streams = []
        for size in sizes:
            needed = (size + block_size - 1) // block_size
            streams.append(blocks(list(struct.unpack_from(f"<{needed}I", directory, at)), size))
            at += 4 * needed
        return streams
    except struct.error as error:
        raise InputError(f"truncated MSF file ({error})") from error


def pdb_identity(data: bytes) -> Identity:
    """A PDB's pdb70 identity: the GUID of its info stream and the age of its DBI stream (the info
    stream's age when there is no DBI stream). InputError for a file that is not a readable PDB."""
    streams = msf_streams(data)
    if len(streams) < 2 or len(streams[1]) < 28:
        raise InputError("the PDB has no info stream")
    age = struct.unpack_from("<I", streams[1], 8)[0]
    if len(streams) > 3 and len(streams[3]) >= 12:
        age = struct.unpack_from("<I", streams[3], 8)[0]
    return Identity("pdb70", guid=format_guid(streams[1][12:28]), age=age)


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command_archive(stage: Path, pdb_dirs: list[Path], output: Path, required: list[str]) -> int:
    """Writes the symbol archive for the PE images directly in `stage` whose PDB is in one of
    `pdb_dirs` (images named in `required` must have one). Prints one line per module; exit 0, or 1
    when a required image is missing or has no matching PDB."""
    if not stage.is_dir():
        raise UsageError(f"{stage} is not a directory")
    modules, failures = [], []
    images = sorted(p for p in stage.iterdir() if p.is_file() and p.suffix.lower() in (".exe", ".dll"))
    for name in required:
        if not (stage / name).is_file():
            failures.append(f"{name}: not in the stage")
    for image in images:
        record = pe_codeview_record(image.read_bytes())
        identity = codeview_identity(record) if record is not None else Identity("none")
        pdb_name = rsds_pdb_name(record) if record is not None else ""
        candidates = [directory / pdb_name for directory in pdb_dirs if pdb_name]
        pdb = next((c for c in candidates if c.is_file()), None)
        problem = ""
        if identity.kind != "pdb70" or not pdb_name:
            problem = "no PDB recorded"
        elif pdb is None:
            problem = f"{pdb_name} not found"
        else:
            try:
                found = pdb_identity(pdb.read_bytes())
                if not found.matches(identity):
                    problem = f"{pdb} is {found.text()}, the image wants {identity.text()}"
            except InputError as error:
                problem = f"{pdb}: {error}"
        if problem:
            if image.name in required:
                failures.append(f"{image.name}: {problem}")
            else:
                print(f"skip   {image.name}: {problem}")
            continue
        modules.append({"image": image.name, "pdb": pdb.name, "identity": identity.text(),
                        "imageSha256": sha256_file(image), "pdbSha256": sha256_file(pdb), "_pdb": pdb, "_image": image})
        print(f"module {image.name} + {pdb.name} ({identity.text()})")
    for failure in failures:
        print(f"FAIL   {failure}")
    if failures:
        return 1
    output.parent.mkdir(parents=True, exist_ok=True)
    partial = output.with_name(output.name + ".part")
    manifest = {"schema": SYMBOL_ARCHIVE_SCHEMA,
                "modules": [{k: v for k, v in m.items() if not k.startswith("_")} for m in modules]}
    with zipfile.ZipFile(partial, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(SYMBOL_ARCHIVE_MANIFEST, json.dumps(manifest, indent=2) + "\n")
        for module in modules:
            archive.write(module["_image"], module["image"])
            archive.write(module["_pdb"], module["pdb"])
    os.replace(partial, output)
    print(f"archive: {output} ({len(modules)} modules)")
    return 0


def unpack_symbol_archive(archive_path: Path, destination: Path) -> Path:
    """Extracts a symbol archive's listed files into `destination` after checking the manifest and
    every file's SHA-256; InputError for anything else (other names, paths, a mismatch)."""
    try:
        with zipfile.ZipFile(archive_path) as archive:
            manifest = json.loads(archive.read(SYMBOL_ARCHIVE_MANIFEST).decode("utf-8"))
            if not isinstance(manifest, dict) or manifest.get("schema") != SYMBOL_ARCHIVE_SCHEMA:
                raise InputError(f"{archive_path}: not a {SYMBOL_ARCHIVE_SCHEMA} archive")
            for module in manifest.get("modules", []):
                for key in ("image", "pdb"):
                    name = module.get(key, "")
                    # Flat names only: nothing may land outside the destination.
                    if not name or name != base_name(name) or name in (".", "..") or ":" in name:
                        raise InputError(f"{archive_path}: bad member name {name!r}")
                    content = archive.read(name)
                    if hashlib.sha256(content).hexdigest() != module.get(key + "Sha256"):
                        raise InputError(f"{archive_path}: {name} does not match its manifest digest")
                    (destination / name).write_bytes(content)
    except (OSError, KeyError, ValueError, zipfile.BadZipFile) as error:
        raise InputError(f"{archive_path}: {error}") from error
    return destination


# --- Inputs ----------------------------------------------------------------------------------


@dataclass
class Module:
    path: str
    base: int
    size: int
    identity: Identity
    origin: str  # "minidump" or "report": how ELF offsets map to virtual addresses (above).

    def contains(self, address: int) -> bool:
        return self.base <= address < self.base + self.size


@dataclass
class Minidump:
    platform: int = 0
    modules: list[Module] = field(default_factory=list)
    exception_thread: int = 0
    exception_code: int = 0
    exception_address: int = 0
    exception_information: list[int] = field(default_factory=list)
    exception_rip: int | None = None
    stacks: dict[int, tuple[int, bytes]] = field(default_factory=dict)  # thread -> (start, bytes)
    annotations: dict[str, str] = field(default_factory=dict)  # crashpad annotation objects
    simple_annotations: dict[str, str] = field(default_factory=dict)  # the handler's --annotation


def read_minidump(data: bytes) -> Minidump:
    """The streams symbolize.py needs from a minidump (MINIDUMP_HEADER and friends)."""
    if data[:4] != b"MDMP":
        raise InputError("not a minidump (no MDMP signature)")
    dump = Minidump()

    def u16(offset: int) -> int:
        if offset < 0 or offset + 2 > len(data):
            raise InputError(f"truncated minidump at 0x{offset:x}")
        return struct.unpack_from("<H", data, offset)[0]

    def u32(offset: int) -> int:
        if offset < 0 or offset + 4 > len(data):
            raise InputError(f"truncated minidump at 0x{offset:x}")
        return struct.unpack_from("<I", data, offset)[0]

    def u64(offset: int) -> int:
        if offset < 0 or offset + 8 > len(data):
            raise InputError(f"truncated minidump at 0x{offset:x}")
        return struct.unpack_from("<Q", data, offset)[0]

    def utf16(rva: int) -> str:  # MINIDUMP_STRING
        length = u32(rva)
        return data[rva + 4 : rva + 4 + length].decode("utf-16-le", errors="replace")

    def utf8(rva: int) -> str:  # crashpad's MinidumpUTF8String / MinidumpByteArray
        length = u32(rva)
        return data[rva + 4 : rva + 4 + length].decode("utf-8", errors="replace")

    def crashpad_info(rva: int) -> None:
        # MinidumpCrashpadInfo: version, report_id, client_id, simple_annotations, module_list.
        simple_size, simple_rva = u32(rva + 36), u32(rva + 40)
        if simple_size >= 4:
            for i in range(u32(simple_rva)):
                key, value = utf8(u32(simple_rva + 4 + i * 8)), utf8(u32(simple_rva + 8 + i * 8))
                dump.simple_annotations[key.split("\0", 1)[0]] = value.split("\0", 1)[0]
        list_size, list_rva = u32(rva + 44), u32(rva + 48)
        if list_size < 4:
            return
        for i in range(u32(list_rva)):
            info = u32(list_rva + 4 + i * 12 + 8)  # {index, {size, rva}}
            # MinidumpModuleCrashpadInfo: version, list_annotations, simple_annotations,
            # annotation_objects (a MinidumpAnnotationList).
            objects_size, objects_rva = u32(info + 20), u32(info + 24)
            if objects_size < 4:
                continue
            for k in range(u32(objects_rva)):
                entry = objects_rva + 4 + k * 12  # {name, type (u16), reserved, value}
                if u16(entry + 4) == 1:  # crashpad::Annotation::Type::kString
                    dump.annotations[utf8(u32(entry)).split("\0", 1)[0]] = utf8(u32(entry + 8))

    streams, directory = u32(8), u32(12)
    for i in range(streams):
        stream_type, size, rva = u32(directory + i * 12), u32(directory + i * 12 + 4), u32(directory + i * 12 + 8)
        if stream_type == 6 and size >= 168:  # ExceptionStream
            dump.exception_thread = u32(rva)
            dump.exception_code = u32(rva + 8)
            dump.exception_address = u64(rva + 24)
            count = min(u32(rva + 32), 15)
            dump.exception_information = [u64(rva + 40 + 8 * k) for k in range(count)]
            dump.exception_rip = u64(u32(rva + 164) + 0xF8)  # CONTEXT_AMD64 Rip
        elif stream_type == 4 and size >= 4:  # ModuleListStream
            for m in range(u32(rva)):
                at = rva + 4 + m * 108
                cv_size, cv_rva = u32(at + 76), u32(at + 80)
                dump.modules.append(Module(utf16(u32(at + 20)), u64(at), u32(at + 8),
                                           codeview_identity(data[cv_rva : cv_rva + cv_size]), "minidump"))
        elif stream_type == 3 and size >= 4:  # ThreadListStream
            for t in range(u32(rva)):
                at = rva + 4 + t * 48
                start, stack_size, stack_rva = u64(at + 24), u32(at + 32), u32(at + 36)
                dump.stacks[u32(at)] = (start, data[stack_rva : stack_rva + stack_size])
        elif stream_type == 7 and size >= 24:  # SystemInfoStream
            dump.platform = u32(rva + 20)
        elif stream_type == 0x43500001 and size >= 52:  # crashpad's MinidumpCrashpadInfo
            crashpad_info(rva)
    return dump


def report_modules(report: dict) -> list[Module]:
    modules = []
    for entry in report.get("modules", []):
        ident = entry.get("id", {})
        identity = Identity(ident.get("kind", "none"), guid=ident.get("guid", ""), age=int(ident.get("age", 0)),
                            build_id=ident.get("hex", ""))
        modules.append(Module(entry.get("path", ""), int(entry.get("base", "0"), 16), int(entry.get("size", 0)),
                              identity, "report"))
    return modules


# --- Symbolization ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Symbol:
    function: str
    file: str
    line: int


@dataclass
class Frame:
    module: str  # The module's file name, or "?".
    offset: int  # From the module base (or the absolute address outside every module).
    symbols: list[Symbol] = field(default_factory=list)  # Innermost inlined function first.
    note: str = ""  # Why the frame is unsymbolized, if it is.

    @property
    def function(self) -> str:
        return self.symbols[0].function if self.symbols else "??"

    def text(self) -> str:
        where = f"{self.module}+0x{self.offset:x}"
        if self.note:
            return f"{where}  ?? ({self.note})"
        top = self.symbols[0]
        inlined = f" (inlined into {self.symbols[-1].function})" if len(self.symbols) > 1 else ""
        location = f"  {base_name(top.file)}:{top.line}" if top.file else ""
        return f"{where}  {top.function}{location}{inlined}"


def plain_path(path: str) -> str:
    """`path` without the Windows extended-length prefix (\\\\?\\ or \\\\?\\UNC\\)."""
    if path.startswith("\\\\?\\UNC\\"):
        return "\\\\" + path[8:]
    if path.startswith("\\\\?\\"):
        return path[4:]
    return path


def base_name(path: str) -> str:
    return plain_path(path).replace("\\", "/").rsplit("/", 1)[-1]


def is_pseudo_module(module: Module) -> bool:
    """A module with no file by design: the Linux vDSO, or an anonymous mapping."""
    name = base_name(module.path)
    return not module.path or module.path.startswith("[") or name.startswith(("linux-vdso.so", "linux-gate.so"))


def parse_symbolizer_output(text: str, count: int) -> list[list[Symbol] | str]:
    """llvm-symbolizer's JSON output for `count` addresses: per address its symbols (innermost
    inlined function first) or an error message. One JSON array for a call's addresses; JSON
    lines (one object per address) are accepted too."""
    try:
        parsed = json.loads(text)
        entries = parsed if isinstance(parsed, list) else [parsed]
    except json.JSONDecodeError:
        entries = []
        for line in text.splitlines():
            if line.strip():
                try:
                    entries.append(json.loads(line))
                except json.JSONDecodeError:
                    entries.append({"Error": {"Message": "unparsable llvm-symbolizer output"}})
    out: list[list[Symbol] | str] = []
    for entry in entries[:count]:
        if not isinstance(entry, dict) or "Error" in entry:
            message = entry.get("Error", {}).get("Message", "error") if isinstance(entry, dict) else "error"
            out.append(f"llvm-symbolizer: {message}")
            continue
        symbols = [Symbol(s.get("FunctionName", "") or "??", s.get("FileName", "") or "", int(s.get("Line", 0) or 0))
                   for s in entry.get("Symbol", []) if isinstance(s, dict)]
        symbols = [s for s in symbols if s.function not in ("??", "")]
        out.append(symbols if symbols else "no symbol")
    out += ["no llvm-symbolizer output"] * (count - len(out))
    return out


class Symbolizer:
    def __init__(self, executable: str, search_dirs: list[Path]) -> None:
        self.executable = executable
        self.search_dirs = search_dirs
        self._located: dict[tuple[str, Identity], tuple[LocalImage | None, str]] = {}

    def locate(self, module: Module) -> tuple[LocalImage | None, str]:
        """The local file for `module` and "" if its identity matches, else (None, reason)."""
        key = (module.path, module.identity)
        if key in self._located:
            return self._located[key]
        candidates = [Path(plain_path(module.path))] if module.path else []
        candidates += [directory / base_name(module.path) for directory in self.search_dirs if module.path]
        result: tuple[LocalImage | None, str] = (None, "module not found")
        for candidate in candidates:
            image = local_image(candidate) if candidate.is_file() else None
            if image is None:
                continue
            if module.identity.kind == "none":
                result = (None, "no identity recorded")
            elif image.identity.matches(module.identity):
                result = (image, "")
                break
            else:
                result = (None, f"identity mismatch (recorded {module.identity.text()}, "
                                f"{candidate} has {image.identity.text()})")
        self._located[key] = result
        return result

    def run(self, command: list[str]) -> str:
        """Runs llvm-symbolizer; its stdout. Tests replace this with a fake."""
        completed = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                                   timeout=300, check=False)
        return completed.stdout

    def symbolize(self, module: Module, offsets: list[int]) -> list[Frame]:
        """One frame per offset from the module base (already adjusted for return addresses)."""
        name = base_name(module.path) or "?"
        image, reason = self.locate(module)
        if image is None:
            return [Frame(name, offset, note=reason) for offset in offsets]
        command = [self.executable, "--output-style=JSON", "--functions=linkage", "--demangle", f"--obj={image.path}"]
        if image.format == "pe":
            command.append("--relative-address")
            addresses = offsets
        else:
            shift = image.file_start_vaddr if module.origin == "minidump" else 0
            addresses = [offset + shift for offset in offsets]
        command += [f"0x{address:x}" for address in addresses]
        try:
            results = parse_symbolizer_output(self.run(command), len(offsets))
        except (OSError, subprocess.TimeoutExpired) as error:
            return [Frame(name, offset, note=f"llvm-symbolizer failed: {error}") for offset in offsets]
        return [Frame(name, offset, symbols=result) if isinstance(result, list) else Frame(name, offset, note=result)
                for offset, result in zip(offsets, results)]

    def frames(self, modules: list[Module], addresses: list[tuple[int, int]]) -> list[Frame]:
        """Frames for (module index or -1, offset) pairs in order, batched per module. Offsets are
        exact; the caller adjusts return addresses."""
        out: list[Frame | None] = [None] * len(addresses)
        by_module: dict[int, list[int]] = {}
        for position, (index, _) in enumerate(addresses):
            by_module.setdefault(index, []).append(position)
        for index, positions in by_module.items():
            if index < 0 or index >= len(modules):
                for position in positions:
                    out[position] = Frame("?", addresses[position][1], note="outside every module")
                continue
            symbolized = self.symbolize(modules[index], [addresses[p][1] for p in positions])
            for position, frame in zip(positions, symbolized):
                out[position] = frame
        return [frame for frame in out if frame is not None]


def find_symbolizer(explicit: str | None) -> str | None:
    if explicit:
        return explicit if Path(explicit).is_file() else None
    suffix = ".exe" if os.name == "nt" else ""
    root = os.environ.get("FROST_LLVM_ROOT")
    if root and (Path(root) / "bin" / f"llvm-symbolizer{suffix}").is_file():
        return str(Path(root) / "bin" / f"llvm-symbolizer{suffix}")
    return shutil.which("llvm-symbolizer")


def module_index(modules: list[Module], address: int) -> int:
    for index, module in enumerate(modules):
        if module.contains(address):
            return index
    return -1


def names_function(name: str, expected: str) -> bool:
    """Whether a symbolized function name is `expected` (a qualified name without parameters):
    llvm-symbolizer prints PDB names bare ("`anonymous namespace'::f") and demangled ELF linkage
    names with parameters ("(anonymous namespace)::f(int)")."""
    return re.search(r"(?:^|[\s:])" + re.escape(expected) + r"(?:$|[\s(])", name) is not None


def same_source(file: str, relative: str) -> bool:
    """Whether the debug-info path `file` is the repository file `relative` (by path suffix, so
    the Windows tree, the WSL ext4 mirror and a worktree all match)."""
    normalized = plain_path(file).replace("\\", "/").lower()
    return normalized.endswith("/" + relative.lower()) or normalized == relative.lower()


def at_marker(frame: Frame, expected: Expectation, lines: set[int], slack: int = 0) -> bool:
    """Whether any of the frame's (inlined) symbols is the expected function within `slack`
    lines of a marker line of its source."""
    return any(names_function(symbol.function, expected.function) and same_source(symbol.file, expected.source)
               and any(abs(symbol.line - line) <= slack for line in lines) for symbol in frame.symbols)


# --- minidump and freeze ---------------------------------------------------------------------


def return_address_offsets(modules: list[Module], addresses: list[int]) -> list[tuple[int, int]]:
    """(module index, offset - 1) for return addresses; (-1, address) outside every module."""
    out = []
    for address in addresses:
        index = module_index(modules, address)
        out.append((index, address - modules[index].base - 1) if index >= 0 else (-1, address))
    return out


def crash_frames(symbolizer: Symbolizer, dump: Minidump) -> tuple[Frame, list[Frame]]:
    """The faulting instruction's frame, then a scan of the crashing thread's stack for values
    that point into a module (candidate return addresses, innermost first, at most 32)."""
    rip = dump.exception_rip or 0
    rip_index = module_index(dump.modules, rip)
    rip_offset = rip - dump.modules[rip_index].base if rip_index >= 0 else rip
    crash = symbolizer.frames(dump.modules, [(rip_index, rip_offset)])[0]
    _, stack = dump.stacks.get(dump.exception_thread, (0, b""))
    values = []
    for at in range(0, len(stack) - 7, 8):
        value = struct.unpack_from("<Q", stack, at)[0]
        if module_index(dump.modules, value) >= 0:
            values.append(value)
        if len(values) == 32:
            break
    scanned = symbolizer.frames(dump.modules, return_address_offsets(dump.modules, values))
    return crash, [frame for frame in scanned if not frame.note]


def annotated_frames(dump: Minidump) -> list[int]:
    """The addresses of the frost.frames annotation ("0x<hex> 0x<hex> ...")."""
    out = []
    for token in dump.annotations.get("frost.frames", "").split():
        try:
            out.append(int(token, 16))
        except ValueError:
            break
    return out


def describe_exception(dump: Minidump) -> str:
    information = (f", information {', '.join(hex(v) for v in dump.exception_information)}"
                   if dump.exception_information else "")
    return (f"exception 0x{dump.exception_code:x} at 0x{dump.exception_address:x} on thread "
            f"{dump.exception_thread}{information}")


def command_minidump(symbolizer: Symbolizer, path: Path) -> int:
    dump = read_minidump(read_input(path))
    crash, scanned = crash_frames(symbolizer, dump)
    print(describe_exception(dump))
    for key in ("frost.fatal_reason", "frost.message", "frost.selftest"):
        if key in dump.annotations:
            print(f"{key}: {dump.annotations[key]}")
    print(f"crash  {crash.text()}")
    frames = symbolizer.frames(dump.modules, return_address_offsets(dump.modules, annotated_frames(dump)))
    for depth, frame in enumerate(frames):
        print(f"frame {depth:2d} {frame.text()}")
    for frame in scanned:
        print(f"scan   {frame.text()}")
    return 0 if not crash.note else 1


def sample_offsets(sample: dict, limit: int | None = None) -> list[tuple[int, int]]:
    """A freeze sample's (module index, offset) pairs; return addresses one byte earlier."""
    out = []
    for depth, (index, offset) in enumerate(sample.get("frames", [])[:limit]):
        value = int(offset, 16)
        out.append((int(index), value - 1 if depth > 0 and value > 0 and int(index) >= 0 else value))
    return out


def freeze_frames(symbolizer: Symbolizer, report: dict, limit: int | None = None) -> list[list[Frame]]:
    """Every sample's frames (at most `limit` per sample), symbolized in one batch per module."""
    modules = report_modules(report)
    flat: list[tuple[int, int]] = []
    shape: list[int] = []
    for sample in report.get("samples", []):
        offsets = sample_offsets(sample, limit)
        shape.append(len(offsets))
        flat += offsets
    symbolized = symbolizer.frames(modules, flat)
    samples, at = [], 0
    for count in shape:
        samples.append(symbolized[at : at + count])
        at += count
    return samples


def command_freeze(symbolizer: Symbolizer, path: Path, every_sample: bool) -> int:
    report = read_json(path)
    if report.get("schema") != FREEZE_SCHEMA:
        raise InputError(f"{path}: not a {FREEZE_SCHEMA} report")
    samples = freeze_frames(symbolizer, report)
    print(f"freeze {report.get('duration_ms')} ms on thread {report.get('thread', {}).get('name')}, "
          f"{len(samples)} samples{', ongoing' if report.get('ongoing') else ''}"
          f"{', unreliable' if report.get('unreliable') else ''}")
    tops = Counter(sample[0].function for sample in samples if sample)
    for function, count in tops.most_common(10):
        print(f"  {count:4d} x {function}")
    for number, sample in enumerate(samples if every_sample else samples[:1]):
        print(f"sample {number}")
        for depth, frame in enumerate(sample):
            print(f"  {depth:2d} {frame.text()}")
    unsymbolized = sum(1 for sample in samples for frame in sample if frame.note)
    return 0 if samples and unsymbolized == 0 else 1


# --- check-selftest --------------------------------------------------------------------------


@dataclass
class Check:
    kind: str
    name: str
    ok: bool
    detail: str

    def text(self) -> str:
        return f"{'ok' if self.ok else 'FAILED'} {self.kind} {self.name}: {self.detail}"


def marker_lines(source_root: Path, expected: Expectation) -> set[int]:
    try:
        lines = (source_root / expected.source).read_text(encoding="utf-8").splitlines()
    except OSError:
        return set()
    return {number for number, text in enumerate(lines, 1) if expected.marker in text}


class SelfTestChecker:
    def __init__(self, symbolizer: Symbolizer, source_root: Path) -> None:
        self.symbolizer = symbolizer
        self.source_root = source_root

    def markers(self, kind: str) -> tuple[Expectation, set[int], Check]:
        expected = EXPECTATIONS[kind]
        lines = marker_lines(self.source_root, expected)
        check = Check(kind, "marker", len(lines) == 1,
                      f"{expected.marker} at {expected.source}:{min(lines)}" if len(lines) == 1 else
                      f"{expected.marker} found {len(lines)} times in {self.source_root / expected.source}")
        return expected, lines, check

    def verify_modules(self, kind: str, modules: list[Module], indexes: set[int]) -> Check:
        """The identity check of every module a checked frame lies in."""
        verified, problems = [], []
        for index in sorted(i for i in indexes if 0 <= i < len(modules)):
            module = modules[index]
            if is_pseudo_module(module):
                continue
            image, reason = self.symbolizer.locate(module)
            if image is None:
                problems.append(f"{base_name(module.path) or '?'}: {reason}")
            else:
                verified.append(f"{base_name(module.path)} ({module.identity.text()})")
        if problems:
            return Check(kind, "modules", False, "; ".join(problems))
        if not verified:
            return Check(kind, "modules", False, "no checked frame lies in a module with a file")
        return Check(kind, "modules", True, ", ".join(verified))

    def crash(self, record: dict, path: Path) -> list[Check]:
        dump = read_minidump(read_input(path))
        expected, lines, marker = self.markers("crash")
        checks = [marker]
        status, signal = int(record.get("status", 0)) & 0xFFFFFFFF, int(record.get("signal", 0))
        if dump.platform == PLATFORM_WINDOWS:
            checks.append(Check("crash", "status", status == EXCEPTION_ACCESS_VIOLATION, f"exit 0x{status:x}"))
            write, address = dump.exception_information[:2] + [None] * (2 - len(dump.exception_information[:2]))
            checks.append(Check("crash", "fault", dump.exception_code == EXCEPTION_ACCESS_VIOLATION and write == 1
                                and address == SELFTEST_CRASH_ADDRESS, describe_exception(dump)))
        elif dump.platform == PLATFORM_LINUX:
            checks.append(Check("crash", "status", signal == LINUX_SIGSEGV, f"signal {signal}"))
            checks.append(Check("crash", "fault", dump.exception_code == LINUX_SIGSEGV
                                and dump.exception_address == SELFTEST_CRASH_ADDRESS, describe_exception(dump)))
        else:
            checks.append(Check("crash", "platform", False, f"unknown minidump platform 0x{dump.platform:x}"))
        if dump.exception_rip is None:
            return checks + [Check("crash", "rip", False, "no exception stream")]
        index = module_index(dump.modules, dump.exception_rip)
        checks.append(self.verify_modules("crash", dump.modules, {index}))
        if index < 0:
            return checks + [Check("crash", "rip", False, f"0x{dump.exception_rip:x} lies in no module")]
        frame = self.symbolizer.frames(dump.modules, [(index, dump.exception_rip - dump.modules[index].base)])[0]
        checks.append(Check("crash", "rip", at_marker(frame, expected, lines), frame.text()))
        return checks

    def contract(self, record: dict, path: Path) -> list[Check]:
        dump = read_minidump(read_input(path))
        expected, lines, marker = self.markers("contract")
        checks = [marker]
        status, signal = int(record.get("status", 0)) & 0xFFFFFFFF, int(record.get("signal", 0))
        if dump.platform == PLATFORM_WINDOWS:
            checks.append(Check("contract", "status", status == FATAL_EXCEPTION_CODE
                                and dump.exception_code == FATAL_EXCEPTION_CODE,
                                f"exit 0x{status:x}, {describe_exception(dump)}"))
        elif dump.platform == PLATFORM_LINUX:
            checks.append(Check("contract", "status", signal == LINUX_SIGABRT and dump.exception_code == LINUX_SIGABRT,
                                f"signal {signal}, {describe_exception(dump)}"))
        else:
            checks.append(Check("contract", "platform", False, f"unknown minidump platform 0x{dump.platform:x}"))
        reason = dump.annotations.get("frost.fatal_reason", "")
        checks.append(Check("contract", "reason", reason == "contract-violation", f"frost.fatal_reason={reason!r}"))
        addresses = annotated_frames(dump)[:CONTRACT_FRAMES]
        if not addresses:
            return checks + [Check("contract", "frames", False, "no frost.frames annotation")]
        offsets = return_address_offsets(dump.modules, addresses)
        checks.append(self.verify_modules("contract", dump.modules, {index for index, _ in offsets}))
        frames = self.symbolizer.frames(dump.modules, offsets)
        hit = next((frame for frame in frames if at_marker(frame, expected, lines)), None)
        checks.append(Check("contract", "frames", hit is not None, hit.text() if hit else
                            f"no {expected.function} at the marker among {len(frames)} frames: "
                            + " | ".join(frame.text() for frame in frames)))
        return checks

    def freeze(self, record: dict, path: Path) -> list[Check]:
        report = read_json(path)
        expected, lines, marker = self.markers("freeze")
        checks = [marker]
        if report.get("schema") != FREEZE_SCHEMA:
            return checks + [Check("freeze", "schema", False, f"{path.name} is not a {FREEZE_SCHEMA} report")]
        status = int(record.get("status", -1))
        checks.append(Check("freeze", "status", status == 0, f"exit {status}"))
        samples = report.get("samples", [])
        duration = int(report.get("duration_ms", 0))
        checks.append(Check("freeze", "report", not report.get("ongoing") and duration >= FREEZE_MIN_DURATION_MS
                            and len(samples) >= FREEZE_MIN_SAMPLES,
                            f"{duration} ms, {len(samples)} samples{', ongoing' if report.get('ongoing') else ''}"))
        modules = report_modules(report)
        indexes = {index for sample in samples for index, _ in sample_offsets(sample, FREEZE_TOP_FRAMES)}
        checks.append(self.verify_modules("freeze", modules, indexes))
        symbolized = freeze_frames(self.symbolizer, report, FREEZE_TOP_FRAMES)
        inside = sum(1 for sample in symbolized
                     if any(at_marker(frame, expected, lines, FREEZE_LINE_SLACK) for frame in sample))
        tops = Counter(sample[0].text() for sample in symbolized if sample).most_common(3)
        checks.append(Check("freeze", "samples", bool(symbolized) and inside * 2 >= len(symbolized),
                            f"{inside} of {len(symbolized)} samples have {expected.function} near its marker in "
                            f"their top {FREEZE_TOP_FRAMES} frames; most frequent: "
                            + " | ".join(f"{count} x {text}" for text, count in tops)))
        return checks

    def check(self, artifacts: Path) -> list[Check]:
        checks: list[Check] = []
        for kind in EXPECTATIONS:
            record_path = artifacts / kind / "artifact.json"
            if not record_path.is_file():
                checks.append(Check(kind, "artifact", False, f"no {record_path}"))
                continue
            try:
                record = read_json(record_path)
                if record.get("schema") != ARTIFACT_SCHEMA or record.get("kind") != kind:
                    raise InputError(f"{record_path}: not a {ARTIFACT_SCHEMA} record of kind {kind}")
                if record.get("marker") not in (None, EXPECTATIONS[kind].marker):
                    raise InputError(f"{record_path}: marker {record.get('marker')!r}, expected "
                                     f"{EXPECTATIONS[kind].marker!r}")
                target = record_path.parent / str(record.get("file", ""))
                checks += getattr(self, kind)(record, target)
            except InputError as error:
                checks.append(Check(kind, "artifact", False, str(error)))
        return checks


def command_check(symbolizer: Symbolizer, artifacts: Path, source_root: Path) -> int:
    if not artifacts.is_dir():
        raise UsageError(f"no artifacts directory {artifacts}")
    checks = SelfTestChecker(symbolizer, source_root).check(artifacts)
    for check in checks:
        print(check.text())
    failed = sum(1 for check in checks if not check.ok)
    print(f"check-selftest: {len(checks)} checks, {failed} failed ({artifacts})")
    return 1 if failed else 0


def read_input(path: Path) -> bytes:
    try:
        return path.read_bytes()
    except OSError as error:
        raise InputError(f"{path}: {error.strerror or error}") from error


def read_json(path: Path) -> dict:
    try:
        value = json.loads(read_input(path).decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise InputError(f"{path}: not JSON ({error})") from error
    if not isinstance(value, dict):
        raise InputError(f"{path}: not a JSON object")
    return value


def main(argv: list[str]) -> int:
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--llvm-symbolizer")
    common.add_argument("--search-dir", action="append", default=[], type=Path)
    common.add_argument("--symbol-archive", action="append", default=[], type=Path,
                        help="a <stage>-symbols.zip from tools/stage.ps1, searched like --search-dir")
    parser = argparse.ArgumentParser(prog="symbolize.py",
                                     description="Symbolizes FrostIDE crash dumps and freeze reports (ADR 0022).")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("minidump", parents=[common]).add_argument("path", type=Path)
    freeze = commands.add_parser("freeze", parents=[common])
    freeze.add_argument("path", type=Path)
    freeze.add_argument("--samples", action="store_true", help="print every sample")
    check = commands.add_parser("check-selftest", parents=[common])
    check.add_argument("--artifacts", type=Path, required=True)
    check.add_argument("--source-root", type=Path, default=REPO)
    archive = commands.add_parser("archive", help="write a stage's symbol archive (FND-13)")
    archive.add_argument("--stage", type=Path, required=True)
    archive.add_argument("--pdb-dir", action="append", type=Path, required=True)
    archive.add_argument("--output", type=Path, required=True)
    archive.add_argument("--require", action="append", default=[], help="an image that must be archived")
    try:
        args = parser.parse_args(argv)
    except SystemExit as error:
        return 2 if error.code else 0
    if args.command == "archive":
        try:
            return command_archive(args.stage, args.pdb_dir, args.output, args.require)
        except (InputError, UsageError, OSError) as error:
            print(f"symbolize.py: {error}", file=sys.stderr)
            return 2
    executable = find_symbolizer(args.llvm_symbolizer)
    if executable is None:
        print("symbolize.py: llvm-symbolizer not found (--llvm-symbolizer, $FROST_LLVM_ROOT/bin or PATH)",
              file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="frost-symbols-") as unpacked:
        search_dirs = list(args.search_dir)
        try:
            for index, symbol_archive in enumerate(args.symbol_archive):
                destination = Path(unpacked) / str(index)
                destination.mkdir()
                search_dirs.append(unpack_symbol_archive(symbol_archive, destination))
            symbolizer = Symbolizer(executable, search_dirs)
            if args.command == "minidump":
                return command_minidump(symbolizer, args.path)
            if args.command == "freeze":
                return command_freeze(symbolizer, args.path, args.samples)
            return command_check(symbolizer, args.artifacts, args.source_root)
        except (InputError, UsageError) as error:
            print(f"symbolize.py: {error}", file=sys.stderr)
            return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
