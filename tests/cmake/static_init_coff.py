# SPDX-License-Identifier: Apache-2.0
#
# Recognizes the exact LLVM 23.1.4 x64 COFF lowering of Asio 1.32.0's already permitted Winsock
# initializer (ADR 0020). This is a closed machine-code signature, not an import allowlist.

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import json
from pathlib import Path
import re
import sys


DATA = "?data_@?$winsock_init@$01$01@detail@asio@@0Udata@winsock_init_base@23@A"
DESTRUCTOR = "??__Fwinsock_init_instance@detail@asio@@YAXXZ"
GUARD = re.compile(r"\?startWinsockManually@\?A0x[0-9A-Fa-f]+@io@frost@@YAXXZ")

# Every instruction is checked, including padding, branch destinations and the stack frame.
# xadd returns the OLD count: WSAStartup is reachable only when that count was zero. The
# earlier manual guard increments this exact counter and never decrements it or registers cleanup.
INITIALIZER_CODE = (
    (0x00, "48 81 ec b8 01 00 00"), (0x07, "b8 01 00 00 00"), (0x0c, "f0"),
    (0x0d, "0f c1 05 00 00 00 00"), (0x14, "85 c0"), (0x16, "74 15"),
    (0x18, "48 8d 0d 00 00 00 00"), (0x1f, "e8 00 00 00 00"), (0x24, "90"),
    (0x25, "48 81 c4 b8 01 00 00"), (0x2c, "c3"), (0x2d, "48 8d 54 24 20"),
    (0x32, "66 b9 02 02"), (0x36, "ff 15 00 00 00 00"), (0x3c, "87 05 04 00 00 00"),
    (0x42, "48 8d 0d 00 00 00 00"), (0x49, "e8 00 00 00 00"), (0x4e, "90"),
    (0x4f, "48 81 c4 b8 01 00 00"), (0x56, "c3"),
)
INITIALIZER_RELOCATIONS = (
    (0x10, "IMAGE_REL_AMD64_REL32", DATA), (0x1b, "IMAGE_REL_AMD64_REL32", DESTRUCTOR),
    (0x20, "IMAGE_REL_AMD64_REL32", "atexit"), (0x38, "IMAGE_REL_AMD64_REL32", "__imp_WSAStartup"),
    (0x3e, "IMAGE_REL_AMD64_REL32", DATA), (0x45, "IMAGE_REL_AMD64_REL32", DESTRUCTOR),
    (0x4a, "IMAGE_REL_AMD64_REL32", "atexit"),
)
GUARD_CODE = (
    (0x00, "b8 01 00 00 00"), (0x05, "f0"), (0x06, "0f c1 05 00 00 00 00"),
    (0x0d, "85 c0"), (0x0f, "74 01"), (0x11, "c3"), (0x12, "31 c0"),
    (0x14, "87 05 04 00 00 00"), (0x1a, "c3"),
)
GUARD_RELOCATIONS = ((0x09, "IMAGE_REL_AMD64_REL32", DATA), (0x16, "IMAGE_REL_AMD64_REL32", DATA))


@dataclass
class Body:
    instructions: list[tuple[int, str]] = field(default_factory=list)
    relocations: list[tuple[int, str, str]] = field(default_factory=list)
    unknown: list[str] = field(default_factory=list)


def bodies(text: str) -> dict[tuple[str, str], Body]:
    """Keep member/function provenance; duplicate definitions and unparsed lines cannot match."""
    result: dict[tuple[str, str], Body] = {}
    member = ""
    body: Body | None = None
    for line in text.splitlines():
        if "file format" in line:
            match = re.fullmatch(r".*\(([^()]*)\):\s+file format coff-x86-64\s*", line)
            member = ""
            if match:
                name = match[1].replace("\\", "/").rsplit("/", 1)[-1]
                member = re.sub(r"\.(?:cpp|cc)\.(?:obj|o)$", "", name)
            body = None
            continue
        match = re.fullmatch(r"[0-9a-fA-F]+ <(.+)>:", line)
        if match:
            key = (member, match[1])
            if key in result:
                raise ValueError(f"duplicate disassembly body: {key}")
            body = Body()
            result[key] = body
            continue
        if not line.strip() or line.startswith("Disassembly of section"):
            body = None if line.startswith("Disassembly of section") else body
            continue
        if body is None:
            continue
        relocation = re.fullmatch(r"\s*([0-9a-fA-F]+):\s+(IMAGE_REL_\S+)\s+(\S+)\s*", line)
        if relocation:
            body.relocations.append((int(relocation[1], 16), relocation[2], relocation[3]))
            continue
        instruction = re.fullmatch(r"\s*([0-9a-fA-F]+):\s+((?:[0-9a-fA-F]{2}\s+)+)(.*)", line)
        if instruction:
            body.instructions.append((int(instruction[1], 16), " ".join(instruction[2].lower().split())))
        else:
            body.unknown.append(line)
    return result


def matches(body: Body, code: tuple, relocations: tuple) -> bool:
    return not body.unknown and tuple(body.instructions) == code and tuple(body.relocations) == relocations


def crt_record(text: str) -> tuple[str, str, str, int, str]:
    fields = text.split("|")
    if len(fields) != 5:
        raise ValueError("invalid CRT relocation record")
    return fields[0], fields[1], fields[2], int(fields[3], 16), fields[4]


def recognize(text: str, guards: list[str], initializers: list[str]) -> list[str]:
    """Return member|initializer keys only after validating the unique earlier manual guard."""
    if len(guards) != 1:
        raise ValueError("exactly one .CRT$XCL relocation must target the reactor Winsock guard")
    guard_member, guard_section, guard_type, guard_offset, guard_target = crt_record(guards[0])
    if (guard_member != "reactor" or guard_section != ".CRT$XCL" or guard_type != "IMAGE_REL_AMD64_ADDR64"
            or guard_offset != 0 or not GUARD.fullmatch(guard_target)):
        raise ValueError(".CRT$XCL must target frost::io's actual startWinsockManually function in reactor")
    functions = bodies(text)
    accepted = []
    for (member, name), body in functions.items():
        if member and name == f"_GLOBAL__sub_I_{member}.cpp" and matches(body, INITIALIZER_CODE, INITIALIZER_RELOCATIONS):
            records = [record for record in map(crt_record, initializers) if record[0] == member and record[4] == name]
            if records != [(member, ".CRT$XCU", "IMAGE_REL_AMD64_ADDR64", 0, name)]:
                raise ValueError("optimized Asio initializer requires exactly one .CRT$XCU entry after the manual guard")
            accepted.append(f"{member}|{name}")
    if accepted:
        guard = functions.get((guard_member, guard_target))
        if guard is None or not matches(guard, GUARD_CODE, GUARD_RELOCATIONS):
            raise ValueError("optimized Asio initialization requires the exact earlier manual increment of the same Winsock counter")
    return accepted


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disassembly", type=Path, required=True)
    parser.add_argument("--guard", action="append", default=[])
    parser.add_argument("--initializer", action="append", default=[])
    args = parser.parse_args()
    try:
        recognized = recognize(args.disassembly.read_text(encoding="utf-8"), args.guard, args.initializer)
    except (OSError, UnicodeError, ValueError) as error:
        print(f"static_init_coff: {error}", file=sys.stderr)
        return 1
    print(json.dumps(recognized))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
