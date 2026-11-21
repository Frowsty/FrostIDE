# SPDX-License-Identifier: Apache-2.0
"""Round-trip and rollback tests for split Linux ELF symbols."""

import os
from pathlib import Path
import shutil
import subprocess
import unittest
from unittest.mock import patch

from tools_test_support import TempDirTestCase
from stage_elf_symbols import inspect, split_stage
from symbolize import Identity, Module, Symbolizer, unpack_symbol_archive


class StageElfSymbolsTest(TempDirTestCase):
    def setUp(self) -> None:
        super().setUp()
        root = os.environ.get("FROST_LLVM_ROOT")
        clang = Path(root) / "bin/clang" if root else shutil.which("clang")
        self.assertIsNotNone(clang, "Load the pinned FrostIDE SDK")
        self.tools = Path(clang).parent
        self.source = self.tmp / "fixture.c"
        self.source.write_text(
            "__attribute__((noinline)) int fixture(int x) { return x + 3; }\n"
            "int main(void) { return fixture(0) != 3; }\n",
            encoding="utf-8",
        )
        self.stage = self.tmp / "stage"
        self.stage.mkdir()
        self.original = self.tmp / "original"
        self.elf = self.stage / "frost.bin"
        self.archive = self.tmp / "stage-symbols.zip"
        self.compile(self.original, "-g")
        shutil.copy2(self.original, self.elf)

    def compile(self, output: Path, debug: str) -> None:
        subprocess.run(
            [self.tools / "clang", debug, "-O0", "-fuse-ld=lld", "-Wl,--build-id=sha1",
             "-o", output, self.source],
            check=True,
        )

    def test_release_elf_recovers_offline_source_line(self) -> None:
        helper = self.stage / "helper"
        self.compile(helper, "-g0")
        helper_bytes = helper.read_bytes()
        original_bytes = self.original.read_bytes()
        build_id, _ = inspect(self.tools / "llvm-readelf", self.elf)
        split_stage(self.stage, self.archive, self.tools, [self.elf.name, helper.name])
        self.assertEqual(inspect(self.tools / "llvm-readelf", self.elf), (build_id, False))
        self.assertEqual(helper.read_bytes(), helper_bytes)
        self.assertEqual(self.original.read_bytes(), original_bytes)
        self.assertNotEqual(self.elf.read_bytes(), original_bytes)
        subprocess.run([self.elf], check=True)

        cache = self.tmp / "symbols"
        cache.mkdir()
        unpack_symbol_archive(self.archive, cache)
        self.assertEqual(sorted(path.name for path in cache.iterdir()), ["frost.bin", "frost.bin.debug"])
        names = subprocess.check_output([self.tools / "llvm-nm", "--defined-only", self.elf], text=True)
        address = next(line.split()[0] for line in names.splitlines() if line.endswith(" T fixture"))
        module = Module(str(self.elf), 0, self.elf.stat().st_size, Identity("gnu-build-id", build_id=build_id), "report")
        symbolizer = Symbolizer(str(self.tools / "llvm-symbolizer"), [cache])
        [frame] = symbolizer.symbolize(module, [int(address, 16)])
        self.assertEqual(frame.note, "")
        self.assertEqual(frame.function, "fixture")
        self.assertEqual(Path(frame.symbols[0].file), self.source)
        self.assertEqual(frame.symbols[0].line, 1)

    def test_archive_publication_failure_restores_stage_and_old_archive(self) -> None:
        self.archive.write_bytes(b"previous archive")
        original_bytes = self.elf.read_bytes()
        replace = os.replace

        def fail_publication(source, target):
            if target == self.archive:
                raise OSError("simulated archive publication failure")
            replace(source, target)

        with patch("stage_elf_symbols.os.replace", side_effect=fail_publication):
            with self.assertRaisesRegex(OSError, "archive publication failure"):
                split_stage(self.stage, self.archive, self.tools, [self.elf.name], replace=True)
        self.assertEqual(self.elf.read_bytes(), original_bytes)
        self.assertEqual(self.archive.read_bytes(), b"previous archive")
        self.assertEqual(list(self.tmp.glob(".frost-elf-symbols-*")), [])

    def test_no_debug_sections_rejects_stale_archive(self) -> None:
        self.compile(self.elf, "-g0")
        original_bytes = self.elf.read_bytes()
        self.archive.write_bytes(b"previous archive")
        with self.assertRaisesRegex(ValueError, "no staged FrostIDE ELF images contain debug sections"):
            split_stage(self.stage, self.archive, self.tools, [self.elf.name], replace=True)
        self.assertEqual(self.elf.read_bytes(), original_bytes)
        self.assertEqual(self.archive.read_bytes(), b"previous archive")


if __name__ == "__main__":
    unittest.main()
