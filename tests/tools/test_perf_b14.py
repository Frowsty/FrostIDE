# SPDX-License-Identifier: Apache-2.0
#
# B14 fixture ownership, incremental fsmonitor admission and desktop input cancellation.

import io
import json
import re
import subprocess
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import MagicMock, patch

from tools_test_support import REPO
import frost_perf


class GitStatusBudgetTest(unittest.TestCase):
    def test_status_arguments_track_the_production_vcs_service(self):
        status = (REPO / "src/vcs/src/git_status.cpp").read_text(encoding="utf-8")
        runner = (REPO / "src/vcs/src/git_runner.cpp").read_text(encoding="utf-8")
        status_body = status.split("statusArguments() {", 1)[1].split("}", 1)[0]
        runner_prefix = runner.split("spec.arguments = {", 1)[1].split("}", 1)[0]
        self.assertEqual(tuple(re.findall(r'"([^"]+)"', runner_prefix + status_body)), frost_perf._B14_COMMAND)

    def test_owned_config_changes_invalidate_fixture(self):
        with tempfile.TemporaryDirectory() as temp:
            work = Path(temp)
            git_dir = work / ".git"
            git_dir.mkdir()
            config = git_dir / "config"
            config.write_text("[core]\nfsmonitor = true\n", encoding="utf-8")
            sha = "2" * 40
            receipt = {"path": str(work), "commit": sha, "configSha256": frost_perf._b14_digest(config)}
            marker = git_dir / frost_perf._B14_MARKER
            marker.write_text(json.dumps(receipt), encoding="utf-8")
            frost_perf._b14_admit(work, sha)
            config.write_text("[core]\nfsmonitor = false\n", encoding="utf-8")
            with self.assertRaisesRegex(frost_perf.RunInvalid, "configuration changed"):
                frost_perf._b14_admit(work, sha)

    def test_witness_requires_successful_incremental_ipc(self):
        response = {"category": "fsm_client", "key": "query/response-length", "value": "31"}
        application = {"category": "fsmonitor", "key": "apply_count", "value": "0"}
        restarted = {"event": "child_start", "child_class": "fsmonitor"}
        for events, stdout, valid in [
            ([response, application], b"clean", True),
            ([response], b"clean", False),
            ([response, application], b"changed", False),
            ([response, application, restarted], b"clean", False),
        ]:
            with self.subTest(events=events), tempfile.TemporaryDirectory() as temp:
                work = Path(temp)
                trace = work / "trace.jsonl"
                trace.write_text("\n".join(map(json.dumps, events)), encoding="utf-8")
                with patch.object(frost_perf, "_b14_git", side_effect=[b"", stdout]):
                    if valid:
                        witness = frost_perf._b14_witness(Path("git.exe"), work, {}, trace, b"clean")
                        self.assertEqual(witness["responseBytes"], 31)
                        self.assertEqual(witness["appliedChanges"], 0)
                    else:
                        with self.assertRaises(frost_perf.RunInvalid):
                            frost_perf._b14_witness(Path("git.exe"), work, {}, trace, b"clean")

    def test_input_cancels_running_git(self):
        proc = MagicMock()
        proc.__enter__.return_value = proc
        proc.poll.return_value = None
        proc.pid = 1234
        proc.communicate.side_effect = subprocess.TimeoutExpired("git", 0.05)
        with (
            patch.object(frost_perf.subprocess, "Popen", return_value=proc),
            patch.object(frost_perf.subprocess, "run") as run,
            patch.object(frost_perf, "_b14_input_tick", side_effect=[1, 1, 2]),
            self.assertRaisesRegex(frost_perf.RunInvalid, "input interrupted"),
        ):
            frost_perf._b14_sample(Path("git.exe"), Path("."), {"SYSTEMROOT": "windows"}, 1, b"clean")
        self.assertEqual(run.call_args.args[0][1:], ["/PID", "1234", "/T", "/F"])
        proc.kill.assert_called_once()

    def test_power_shell_options_route_with_standard_runner(self):
        args = frost_perf.parse_args(["-Budget", "B14", "-B14Fixture", "f", "-B14Git", "g",
                                      "-B14SessionProbe", "p"])
        self.assertEqual((args.budget, args.b14_fixture, args.b14_git, args.b14_session_probe),
                         ("B14", "f", "g", "p"))

    def test_p50_uses_arithmetic_median_for_even_sample_count(self):
        args = frost_perf.parse_args(["-Budget", "B14", "-B14Fixture", "f", "-B14Git", "g",
                                      "-B14SessionProbe", "p"])
        budget = frost_perf.load_budget(REPO / frost_perf.BUDGETS_FILE, "B14")
        with tempfile.TemporaryDirectory() as temp:
            work = Path(temp)
            for key, name in (("b14_git", "git.exe"), ("b14_session_probe", "s1_probe_validate.exe")):
                path = work / name
                path.touch()
                setattr(args, key, str(path))
            (work / ".git").mkdir()
            args.b14_fixture = str(work)
            samples = {"samplesMs": [140.0] * 10 + [170.0] * 10}
            output = io.StringIO()
            with (
                patch.object(frost_perf.os, "name", "nt"),
                patch.object(frost_perf, "Path", type(work)),
                patch.object(frost_perf, "_b14_admit"),
                patch.object(frost_perf, "_b14_measure", return_value=samples),
                redirect_stdout(output),
            ):
                self.assertEqual(frost_perf.run_b14(args, REPO, budget), frost_perf.EXIT_OVER)
            self.assertEqual(json.loads(output.getvalue())["statisticsMs"], {"p50": 155.0, "p99": 170.0})


if __name__ == "__main__":
    unittest.main()
