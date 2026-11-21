# SPDX-License-Identifier: Apache-2.0
#
# Verify B1 native capture admission and input cancellation without an interactive display.

import copy
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import MagicMock, patch

import tools_test_support
import perf_b1


class FirstFrameTest(unittest.TestCase):
    def setUp(self):
        self.report = {
            "tool": "s1_first_frame",
            "target": "frost.exe",
            "arguments": "",
            "mode": "warm",
            "status": "valid",
            "sessionStart": {"valid": True},
            "sessionEnd": {"valid": True},
            "accessLost": 0,
            "captureFailure": "",
            "launches": [
                {"priming": n == 0, "valid": True, "firstFrameMs": n + 0.5}
                for n in range(11)
            ],
        }

    def test_discards_only_priming_launch(self):
        self.assertEqual(perf_b1.read_samples(self.report), [n + 0.5 for n in range(1, 11)])

    def test_incomplete_or_invalid_series_is_refused(self):
        for patch_values in [
            {"sessionStart": {"valid": False}},
            {"sessionEnd": {"valid": False}},
            {"mode": "cold-proxy"},
            {"arguments": "--project elsewhere"},
            {"accessLost": 1},
            {"captureFailure": "display disconnected"},
            {"launches": self.report["launches"][:-1]},
        ]:
            with self.subTest(patch_values=patch_values), self.assertRaises(ValueError):
                perf_b1.read_samples(self.report | patch_values)
        for bad_sample in (None, float("nan"), -1, False):
            report = copy.deepcopy(self.report)
            report["launches"][4]["firstFrameMs"] = bad_sample
            with self.subTest(bad_sample=bad_sample), self.assertRaises(ValueError):
                perf_b1.read_samples(report)

    def test_input_while_probe_is_running_kills_probe(self):
        process = MagicMock()
        process.__enter__.return_value = process
        process.poll.return_value = None
        with (
            tempfile.TemporaryDirectory() as temp,
            patch.object(perf_b1.subprocess, "Popen", return_value=process),
            patch.object(perf_b1, "last_input_tick", side_effect=[1, 2]),
            self.assertRaisesRegex(RuntimeError, "input during"),
        ):
            perf_b1.capture_block(Path("probe"), Path("frost"), Path(temp) / "raw.json", {}, 1)
        process.kill.assert_called_once()

    def test_component_summary_requires_two_complete_blocks(self):
        for error in (None, RuntimeError("capture interrupted")):
            with self.subTest(error=error), tempfile.TemporaryDirectory() as temp:
                probe, frost, out = (Path(temp) / name for name in ("s1_first_frame.exe", "frost.exe", "new"))
                probe.write_bytes(b"probe fixture")
                frost.write_bytes(b"frost fixture")
                app = frost.with_name("frost_app.dll")
                app.write_bytes(b"frost app fixture")
                argv = ["perf_b1.py", "--probe", str(probe), "--frost", str(frost), "--out-dir", str(out)]
                samples = perf_b1.read_samples(self.report)
                with (
                    patch.object(perf_b1.sys, "argv", argv),
                    patch.object(perf_b1.sys, "platform", "win32"),
                    patch.object(perf_b1, "last_input_tick", return_value=7),
                    patch.object(perf_b1, "capture_block", side_effect=[samples, error or samples]) as capture,
                ):
                    self.assertEqual(perf_b1.main(), 3 if error else 0)
                summary = json.loads((out / "summary.json").read_text(encoding="utf-8"))
                self.assertEqual(summary["scope"], "component-only")
                self.assertEqual(summary["sha256"]["frost"], perf_b1.file_digest(frost))
                self.assertEqual(summary["sha256"]["frost_app"], perf_b1.file_digest(app))
                self.assertEqual(capture.call_count, 2)
                env = capture.call_args.args[3]
                self.assertEqual(Path(env["APPDATA"]), out / "profile")
                self.assertEqual(Path(env["LOCALAPPDATA"]), out / "profile")
                if error:
                    self.assertEqual(summary["status"], "invalid")
                    self.assertNotIn("samplesMs", summary)
                else:
                    self.assertEqual(summary["status"], "measured")
                    self.assertEqual(len(summary["samplesMs"]), 20)


if __name__ == "__main__":
    unittest.main()
