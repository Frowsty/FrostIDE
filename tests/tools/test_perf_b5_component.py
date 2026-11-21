# SPDX-License-Identifier: Apache-2.0
#
# B5 component receipt validation and separate backend, wrap and viewport groups.

import hashlib
from pathlib import Path
import unittest

from tools_test_support import TempDirTestCase, run_tool

import perf_b5_component


class ComponentTest(TempDirTestCase):
    def test_binds_native_log_and_does_not_pool_configuration_or_files(self):
        expected_file = "a" * 64
        other_file = "b" * 64
        rows = [
            f"endpoint=raster-paint-return wrap=false elapsed_ns=3000000 text_version=1 viewport=500x400 file_id={expected_file}",
            f"endpoint=raster-paint-return wrap=false elapsed_ns=1000000 text_version=2 viewport=500x400 file_id={expected_file}",
            f"endpoint=qrhi-display-list wrap=true elapsed_ns=2000000 text_version=3 viewport=600x400 file_id={expected_file}",
            f"endpoint=raster-paint-return wrap=false elapsed_ns=9000000 text_version=1 viewport=500x400 file_id={other_file}",
        ]
        log = self.tmp / "native.log"
        log.write_text("".join(f"2026-11-21T01:01:01.000000Z [info] [perf.b5/123] {r}\n" for r in rows),
                       encoding="utf-8")
        digest = hashlib.sha256(log.read_bytes()).hexdigest()
        receipt = perf_b5_component.component(log, digest, expected_file)
        self.assertEqual(receipt["scope"], "component-only")
        self.assertNotIn("verdict", receipt)
        self.assertEqual(len(receipt["series"]), 2)
        qrhi, raster = receipt["series"]
        self.assertEqual((qrhi["endpoint"], qrhi["wrap"], qrhi["samplesMs"]),
                         ("qrhi-display-list", True, [2.0]))
        self.assertEqual((raster["endpoint"], raster["wrap"], raster["viewportPixels"]),
                         ("raster-paint-return", False, [500.0, 400.0]))
        self.assertEqual((raster["medianMs"], raster["p99Ms"]), (2.0, 3.0))
        process = run_tool("perf_b5_component.py", "--log", str(log), "--log-sha256", "e" * 64,
                           "--file-id", expected_file)
        self.assertEqual(process.returncode, 3)
        self.assertIn("SHA-256", process.stdout)
        with self.assertRaisesRegex(ValueError, "no matching"):
            perf_b5_component.component(log, digest, "c" * 64)


if __name__ == "__main__":
    unittest.main()
