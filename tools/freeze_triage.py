#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# freeze_triage.py: groups FrostIDE's freeze reports (frost-freeze-report/1, frost/diag/freeze_report.hpp)
# by the FrostIDE code that stalled the UI thread, against the interim stall policy (ADR 0078 decision 7:
# 50 ms or more is recorded, over 250 ms fails the milestone journeys; FND-9b).
#
#   python -I tools/freeze_triage.py triage <report or directory>... [--json <file>] [--limit-ms 250]
#                                    [--llvm-symbolizer <path>] [--search-dir <dir>]...
#   python -I tools/freeze_triage.py check <report or directory>... [--limit-ms 250]
#
# triage symbolizes every sample of every report with tools/symbolize.py (the module identities the
# report recorded are checked first; a module whose PDB or build ID does not match stays
# unsymbolized) and puts each report in one bucket: the FrostIDE function (a frame in a module named
# frost*) innermost in most of its samples, or, when no FrostIDE frame symbolizes, the innermost
# FrostIDE frame's module and offset with the reason ("unsymbolized ..."). It prints the buckets with
# the reports over the limit first: count over the limit, count, longest duration, the versions, the
# chain of distinct FrostIDE functions from that frame outwards (the handler that ran it) and one
# report's file name. --json writes the same as {"limitMs", "reports": [...], "buckets": [...]}.
# check prints every report longer than the limit (no symbolization) and exits 1 when there is one:
# the gate the journeys apply to the reports of their own FrostIDE profile.
#
# Exit status: 0 (triage: always when the input was read; check: no report over the limit), 1 (check:
# a report over the limit), 2 (usage errors, unreadable input, triage without llvm-symbolizer).
# Python 3.11+, standard library only; run with -I.

from __future__ import annotations

import argparse
import bisect
import json
import struct
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import symbolize  # noqa: E402  (beside this file)

LIMIT_MS = 250  # ADR 0078 decision 7, the interim gate.
CHAIN_LENGTH = 6  # FrostIDE functions shown per bucket, innermost first.
# FrostIDE frames that only mean "the event loop ran it": a stall whose innermost FrostIDE frame is one
# of these is inside Qt or the OS, and its bucket names the innermost frame there too.
EVENT_LOOP_ENTRIES = ("`anonymous namespace'::run", "frost_main", "main", "wmain")
# Modules whose frames are the OS waiting, skipped when naming where inside Qt a stall is.
OS_WAITS = ("ntdll.dll", "kernelbase.dll", "kernel32.dll", "win32u.dll", "libc.so.6")


@dataclass
class Report:
    path: Path
    duration_ms: int
    version: str
    ongoing: bool
    unreliable: bool
    started: str
    data: dict


@dataclass
class Bucket:
    key: str
    chain: list[str] = field(default_factory=list)
    reports: list[Report] = field(default_factory=list)

    def over(self, limit_ms: int) -> int:
        return sum(1 for report in self.reports if report.duration_ms > limit_ms)

    def longest(self) -> int:
        return max((report.duration_ms for report in self.reports), default=0)


def report_files(inputs: list[str]) -> list[Path]:
    """The freeze-*.json files named or inside the named directories, sorted by name."""
    files: list[Path] = []
    for item in inputs:
        path = Path(item)
        if path.is_dir():
            files += sorted(path.glob("freeze-*.json"))
        elif path.is_file():
            files.append(path)
        else:
            raise symbolize.InputError(f"{path}: no such file or directory")
    return files


def read_report(path: Path) -> Report:
    data = symbolize.read_json(path)
    if data.get("schema") != symbolize.FREEZE_SCHEMA:
        raise symbolize.InputError(f"{path}: not a {symbolize.FREEZE_SCHEMA} report")
    return Report(path, int(data.get("duration_ms", 0)), str(data.get("version", "")), bool(data.get("ongoing")),
                  bool(data.get("unreliable")), str(data.get("started_utc", "")), data)


def pe_exports(data: bytes) -> list[tuple[int, str]]:
    """The named exports of a PE32/PE32+ image as (RVA, name), sorted by RVA (Microsoft's PE format
    documentation, the export directory: AddressOfFunctions, AddressOfNames, AddressOfNameOrdinals).
    Forwarders (an RVA inside the export directory) are left out. [] when there are none."""
    try:
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe : pe + 4] != b"PE\0\0":
            return []
        sections, optional_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
        optional = pe + 24
        magic = struct.unpack_from("<H", data, optional)[0]
        directories = optional + (112 if magic == 0x20B else 96)
        export_rva, export_size = struct.unpack_from("<II", data, directories)
        table = optional + optional_size

        def offset(rva: int) -> int:
            for i in range(sections):
                virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from("<IIII", data,
                                                                                          table + i * 40 + 8)
                if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
                    return rva - virtual_address + raw_pointer
            raise ValueError(f"RVA 0x{rva:x} is in no section")

        if not export_size:
            return []
        directory = offset(export_rva)
        # IMAGE_EXPORT_DIRECTORY: NumberOfNames, AddressOfFunctions, AddressOfNames, AddressOfNameOrdinals.
        count_names, functions, names, ordinals = struct.unpack_from("<IIII", data, directory + 24)
        function_table, name_table, ordinal_table = offset(functions), offset(names), offset(ordinals)
        out: list[tuple[int, str]] = []
        for i in range(count_names):
            name_at = offset(struct.unpack_from("<I", data, name_table + 4 * i)[0])
            name = data[name_at : data.index(b"\0", name_at)].decode("ascii", errors="replace")
            ordinal = struct.unpack_from("<H", data, ordinal_table + 2 * i)[0]
            rva = struct.unpack_from("<I", data, function_table + 4 * ordinal)[0]
            if not export_rva <= rva < export_rva + export_size:
                out.append((rva, name))
        return sorted(out)
    except (struct.error, ValueError):
        return []


def readable_export(name: str) -> str:
    """An MSVC-decorated export's qualified name ("?exec@QDrag@@QEAA..." is "QDrag::exec"); other
    names as they are. Only the name part is read; the signature is dropped."""
    if not name.startswith("?") or name.startswith("??"):
        return name
    parts = name[1:].split("@@", 1)[0].split("@")
    return "::".join(reversed([part for part in parts if part]))


class ExportFallbackSymbolizer(symbolize.Symbolizer):
    """symbolize.Symbolizer, plus: a frame in a PE module whose file matches the recorded identity but
    has no PDB here (Qt's release DLLs, the OS) is named after the nearest export at or below it,
    "<export>+0x<delta> [export]". The module identity is still checked first (locate)."""

    def __init__(self, executable: str, search_dirs: list[Path]) -> None:
        super().__init__(executable, search_dirs)
        self._exports: dict[Path, list[tuple[int, str]]] = {}

    def symbolize(self, module: symbolize.Module, offsets: list[int]) -> list[symbolize.Frame]:
        frames = super().symbolize(module, offsets)
        if not any(frame.note for frame in frames):
            return frames
        image, _ = self.locate(module)
        if image is None or image.format != "pe":
            return frames
        if image.path not in self._exports:
            try:
                self._exports[image.path] = pe_exports(image.path.read_bytes())
            except OSError:
                self._exports[image.path] = []
        exports = self._exports[image.path]
        rvas = [rva for rva, _ in exports]
        for frame in frames:
            at = bisect.bisect_right(rvas, frame.offset) - 1
            if frame.note and at >= 0:
                rva, name = exports[at]
                frame.symbols = [symbolize.Symbol(f"{readable_export(name)}+0x{frame.offset - rva:x} [export]", "", 0)]
                frame.note = ""
        return frames


def is_frost(frame: symbolize.Frame) -> bool:
    return frame.module.lower().startswith("frost")


def classify(samples: list[list[symbolize.Frame]]) -> tuple[str, list[str]]:
    """The bucket key and FrostIDE function chain of one report's symbolized samples."""
    innermost: Counter[str] = Counter()
    chains: dict[str, list[str]] = {}
    unsymbolized: Counter[str] = Counter()
    for sample in samples:
        frost = [frame for frame in sample if is_frost(frame)]
        named = [frame for frame in frost if not frame.note]
        if named:
            key = named[0].function
            innermost[key] += 1
            if key not in chains:
                chain: list[str] = []
                for frame in named:
                    if frame.function not in chain:
                        chain.append(frame.function)
                chains[key] = chain[:CHAIN_LENGTH]
        elif frost:
            unsymbolized[f"unsymbolized {frost[0].module}+0x{frost[0].offset:x} ({frost[0].note})"] += 1
        else:
            unsymbolized["no FrostIDE frame"] += 1
    if innermost:
        key, _ = innermost.most_common(1)[0]
        if any(key.startswith(entry) for entry in EVENT_LOOP_ENTRIES):
            # The stall is below the event loop, in Qt or the OS: name the innermost Qt-level frame.
            inside: Counter[str] = Counter()
            for sample in samples:
                named = [f for f in sample if not f.note and not is_frost(f) and f.module.lower() not in OS_WAITS]
                if named:
                    inside[f"{named[0].module}!{named[0].function}"] += 1
            if inside:
                return f"{key} > {inside.most_common(1)[0][0]}", chains[key]
        return key, chains[key]
    if unsymbolized:
        return unsymbolized.most_common(1)[0][0], []
    return "no samples", []


def triage(symbolizer: symbolize.Symbolizer, reports: list[Report]) -> list[Bucket]:
    buckets: dict[str, Bucket] = {}
    for report in reports:
        key, chain = classify(symbolize.freeze_frames(symbolizer, report.data))
        bucket = buckets.setdefault(key, Bucket(key, chain))
        bucket.reports.append(report)
    return list(buckets.values())


def ordered(buckets: list[Bucket], limit_ms: int) -> list[Bucket]:
    return sorted(buckets, key=lambda b: (-b.over(limit_ms), -len(b.reports), -b.longest(), b.key))


def print_triage(buckets: list[Bucket], reports: list[Report], limit_ms: int) -> None:
    over = sum(1 for report in reports if report.duration_ms > limit_ms)
    print(f"{len(reports)} freeze reports, {over} over {limit_ms} ms, {len(buckets)} buckets")
    for bucket in ordered(buckets, limit_ms):
        versions = ", ".join(sorted({report.version or "?" for report in bucket.reports}))
        example = max(bucket.reports, key=lambda r: r.duration_ms).path.name
        print(f"{bucket.over(limit_ms):4d} over {limit_ms} ms, {len(bucket.reports):4d} reports, longest "
              f"{bucket.longest():6d} ms  {bucket.key}")
        print(f"       versions: {versions}; longest: {example}")
        if len(bucket.chain) > 1:
            print("       chain: " + " <- ".join(bucket.chain[1:]))


def triage_json(buckets: list[Bucket], reports: list[Report], limit_ms: int) -> dict:
    return {"limitMs": limit_ms,
            "reports": [{"file": r.path.name, "durationMs": r.duration_ms, "version": r.version, "started": r.started,
                         "ongoing": r.ongoing, "unreliable": r.unreliable} for r in reports],
            "buckets": [{"key": b.key, "chain": b.chain, "reports": [r.path.name for r in b.reports],
                         "overLimit": b.over(limit_ms), "longestMs": b.longest()}
                        for b in ordered(buckets, limit_ms)]}


def over_limit(reports: list[Report], limit_ms: int) -> list[Report]:
    return sorted((r for r in reports if r.duration_ms > limit_ms), key=lambda r: -r.duration_ms)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="freeze_triage.py", description="Triage FrostIDE freeze reports.")
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("triage", "check"):
        command = commands.add_parser(name)
        command.add_argument("inputs", nargs="+", help="freeze reports or directories holding freeze-*.json")
        command.add_argument("--limit-ms", type=int, default=LIMIT_MS)
        if name == "triage":
            command.add_argument("--json", help="also write the triage as JSON to this file")
            command.add_argument("--llvm-symbolizer")
            command.add_argument("--search-dir", action="append", default=[])
    try:
        args = parser.parse_args(argv)
    except SystemExit as stop:
        return 2 if stop.code else 0
    try:
        reports = [read_report(path) for path in report_files(args.inputs)]
    except symbolize.InputError as error:
        print(f"freeze_triage.py: {error}", file=sys.stderr)
        return 2
    if args.command == "check":
        over = over_limit(reports, args.limit_ms)
        for report in over:
            print(f"over {args.limit_ms} ms: {report.path.name} {report.duration_ms} ms"
                  f"{' (ongoing)' if report.ongoing else ''}")
        print(f"{len(reports)} freeze reports, {len(over)} over {args.limit_ms} ms")
        return 1 if over else 0
    executable = symbolize.find_symbolizer(args.llvm_symbolizer)
    if executable is None:
        print("freeze_triage.py: llvm-symbolizer not found (--llvm-symbolizer, FROST_LLVM_ROOT, PATH)", file=sys.stderr)
        return 2
    symbolizer = ExportFallbackSymbolizer(executable, [Path(d) for d in args.search_dir])
    buckets = triage(symbolizer, reports)
    print_triage(buckets, reports, args.limit_ms)
    if args.json:
        Path(args.json).write_text(json.dumps(triage_json(buckets, reports, args.limit_ms), indent=2) + "\n",
                                   encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
