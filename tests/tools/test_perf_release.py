# SPDX-License-Identifier: Apache-2.0
#
# Full performance CLI admission, authenticated evidence and comparative statistics.

import hashlib
from itertools import combinations
import json
from pathlib import Path
import shutil
import signal
import sys
import tempfile
import time
import unittest
from unittest.mock import Mock, patch

from tools_test_support import REPO, TempDirTestCase, git, run_tool
import frost_perf
import perf_release
import test_frost_perf


def digest(data):
    return hashlib.sha256(data).hexdigest()


class FullPerformanceTest(TempDirTestCase):
    def setUp(self):
        super().setUp()
        self.repo = self.tmp / "repo"
        paths = {
            "lockSha256": "sdk/sdk.lock.json",
            "budgetsSha256": "tests/perf/budgets.json",
            "corporaSha256": "tests/corpus/manifest.json",
            "runner": "tools/frost_perf.py",
        }
        self.identity = {}
        for key, name in paths.items():
            target = self.repo / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPO / name, target)
            self.identity[key] = digest(target.read_bytes())
        self.runner_sha = self.identity.pop("runner")
        self.pin = next(item["sha"] for item in json.loads(
            (self.repo / paths["corporaSha256"]).read_text(encoding="utf-8")
        )["corpora"] if item["id"] == "R3")
        git("init", cwd=self.repo)
        git("add", "--all", cwd=self.repo)
        git("commit", "-m", "Baseline", cwd=self.repo)
        self.predecessor = git("rev-parse", "HEAD", cwd=self.repo).strip()
        (self.repo / "candidate.txt").write_text("candidate\n", encoding="utf-8")
        git("add", "candidate.txt", cwd=self.repo)
        git("commit", "-m", "Candidate", cwd=self.repo)
        self.identity["commit"] = git("rev-parse", "HEAD", cwd=self.repo).strip()
        self.environment = self.write("environment.json", {
            "profile": "REF-W1", "os": "windows", "logicalThreads": 24, "memoryGiB": 32,
            "gpu": "NVIDIA GeForce RTX 4070 SUPER", "display": [5120, 1440, 144],
        })
        self.plan = {"schemaVersion": 1, "identity": self.identity, "environment": self.environment,
                     "reviewer": "independent test reviewer", "budgets": {}}

    def write(self, filename, document):
        path = self.tmp / filename
        path.write_text(json.dumps(document), encoding="utf-8")
        return {"file": filename, "sha256": digest(path.read_bytes())}

    def cli(self, anchored=True, wrong_hash=False):
        args = ["-Full", "--repo", str(self.repo)]
        if anchored:
            manifest = self.write("manifest.json", self.plan)
            args += ["-FullManifest", str(self.tmp / manifest["file"]),
                     "-FullManifestSha256", "f" * 64 if wrong_hash else manifest["sha256"]]
        result = run_tool("frost_perf.py", *args)
        self.assertEqual(result.returncode, frost_perf.EXIT_INVALID, result.stdout + result.stderr)
        return json.loads(result.stdout)

    def receipt(self, old=False, **overrides):
        session = {
            "session": {"valid": True}, "inputDesktop": "Default", "lockState": "unlocked",
            "displayPower": "on", "processSession": 1, "activeConsoleSession": 1,
            "outputs": [{"attached": True, "width": 5120, "height": 1440, "refreshHz": 144}],
        }
        native = {
            "budget": "B14", "corpus": "R3", "corpusCommit": self.pin, "scope": "absolute-budget-only",
            "state": "within", "inputUnchanged": True, "gitCommand": list(frost_perf._B14_COMMAND),
            "workTree": "E:/perf-fixture/private-R3",
            "gitSha256": "a" * 64, "gitRuntimeSha256": "b" * 64, "sessionProbeSha256": "c" * 64,
            "indexSha256": "d" * 64, "runnerSha256": self.runner_sha,
            "statusSha256": digest(f"# branch.oid {self.pin}\0# branch.head (detached)\0".encode()),
            "budgetManifestSha256": self.identity["budgetsSha256"],
            "corpusManifestSha256": self.identity["corporaSha256"],
            "sessionStart": session, "sessionEnd": session,
            "fsmonitorStart": {"traceSha256": "c" * 64, "responseBytes": 42, "appliedChanges": 0},
            "fsmonitorEnd": {"traceSha256": "d" * 64, "responseBytes": 42, "appliedChanges": 0},
            "samplesMs": [100.0 if old else 108.0] * 20,
        }
        identity = dict(self.identity, commit=self.predecessor) if old else self.identity
        return {
            "schemaVersion": 1, "kind": "B14-native", "identity": identity, "native": native,
            "environmentSha256": self.environment["sha256"], "precheckStart": True, "precheckEnd": True,
            "inputUnchanged": True, "quiet": True, "concurrentBuilds": False,
            "startUtc": "2026-11-20T01:00:00Z" if old else "2026-11-21T01:00:00Z",
            "endUtc": "2026-11-20T01:01:00Z" if old else "2026-11-21T01:01:00Z",
            **overrides,
        }

    def b14(self, candidate):
        self.plan["budgets"]["B14"] = {"current": self.write("candidate.json", candidate),
                                       "baseline": self.write("baseline.json", self.receipt(old=True))}

    def test_no_receipts_reports_all_28_blocked(self):
        result = self.cli(anchored=False)
        self.assertEqual(result["status"], "incomplete")
        self.assertEqual(set(result["budgets"]), {f"B{i}" for i in range(1, 29)})
        self.assertTrue(all(row["state"] == "blocked" for row in result["budgets"].values()))

    def test_anchor_and_release_source_must_match(self):
        self.assertIn("anchor", self.cli(wrong_hash=True)["reason"])
        self.plan["identity"] = dict(self.identity, commit=self.predecessor)
        self.assertIn("source", self.cli()["reason"])
        self.plan["identity"] = self.identity
        (self.repo / "candidate.txt").write_text("changed", encoding="utf-8")
        self.assertIn("tracked local changes", self.cli()["reason"])

    def test_exact_evidence_detects_significant_regression_below_absolute_budget(self):
        self.b14(self.receipt())
        outcome = self.cli()["budgets"]["B14"]
        self.assertEqual(outcome["state"], "over")
        self.assertFalse(outcome["absoluteExceeded"])
        self.assertTrue(outcome["comparativeExceeded"])
        self.assertAlmostEqual(outcome["pValue"], 2 / 137846528820)
        (self.tmp / "candidate.json").write_text("{}", encoding="utf-8")
        self.assertIn("SHA-256", self.cli()["budgets"]["B14"]["reason"])

    def test_rejects_contention_mismatched_environment_and_wrong_workload(self):
        for field, value, message in (
            ("concurrentBuilds", True, "contended"),
            ("environmentSha256", "e" * 64, "environment"),
            ("inputUnchanged", False, "input"),
            ("identity", dict(self.identity, commit=self.predecessor), "source"),
        ):
            with self.subTest(field=field):
                self.b14(self.receipt(**{field: value}))
                outcome = self.cli()["budgets"]["B14"]
                self.assertEqual(outcome["state"], "invalid")
                self.assertIn(message, outcome["reason"])
        envelope = self.receipt()
        envelope["native"]["gitCommand"] = ["status"]
        self.b14(envelope)
        self.assertIn("workload", self.cli()["budgets"]["B14"]["reason"])
        envelope = self.receipt()
        envelope["native"]["workTree"] = "C:/perf/other-ssd"
        self.b14(envelope)
        self.assertIn("E: volume", self.cli()["budgets"]["B14"]["reason"])
        envelope = self.receipt()
        envelope["native"]["sessionEnd"].pop("processSession")
        self.b14(envelope)
        self.assertEqual(self.cli()["budgets"]["B14"]["state"], "invalid")

    def test_baseline_must_match_the_actual_historical_lock_blob(self):
        path = self.repo / "sdk/sdk.lock.json"
        path.write_bytes(path.read_bytes() + b"\n")
        self.identity["lockSha256"] = digest(path.read_bytes())
        git("add", "sdk/sdk.lock.json", cwd=self.repo)
        git("commit", "-m", "Different lock bytes", cwd=self.repo)
        self.identity["commit"] = git("rev-parse", "HEAD", cwd=self.repo).strip()
        self.b14(self.receipt())
        self.assertIn("sdk/sdk.lock.json bytes", self.cli()["budgets"]["B14"]["reason"])

    def test_full_executes_existing_native_b27_collector(self):
        installed, archive = test_frost_perf.InstalledSizeTest.fixture(self)
        self.plan["budgets"]["B27"] = {
            "identity": self.identity, "platform": "windows",
            "installed": installed.name, "archive": archive.name, "archiveSha256": digest(archive.read_bytes()),
        }
        result = self.cli()
        self.assertEqual(result["status"], "incomplete")
        outcome = result["budgets"]["B27"]
        if frost_perf.os.name != "nt":
            self.assertEqual(outcome["state"], "invalid")
            self.assertIn("native Windows", outcome["reason"])
            return
        self.assertEqual((outcome["state"], outcome["source"]), ("within", "live-native-storage"))
        self.assertGreater(outcome["native"]["allocatedBytes"], 0)
        self.assertEqual(outcome["native"]["limitBytes"], 400_000_000)




    def b20_native(self, old=False, tamper=None):
        label = "baseline" if old else "candidate"
        envelope = self.receipt(old=old, kind="B20-native")
        session = envelope["native"]["sessionStart"]
        workspace = "E:/perf-fixture/R3/llvm"
        samples = [
            {"tool": "s1_exit", "pid": 100 + index, "workspace": workspace, "exit": 0,
             "sessionIndex": f"E:/perf-fixture/{label}/profile-{index:02d}/FrostIDE/data/sessions/index.json",
             "exitMs": 100.0 if old else 110.0}
            for index in range(20)
        ]
        native = {
            "budget": "B20", "corpus": "R3", "corpusCommit": self.pin, "workload": "trusted-r3-wm-close",
            "state": "within", "inputUnchanged": True, "workspace": workspace, "samples": samples,
            "startUtc": envelope["startUtc"], "endUtc": envelope["endUtc"],
            "sessionStart": session, "sessionEnd": session, "initialCpuPct": 2.0, "finalCpuPct": 4.0,
            "imagesSha256": {key: "a" * 64 for key in ("frost", "app", "exitProbe", "sessionProbe")},
            "runnerSha256": self.runner_sha, "trustSha256": "b" * 64,
        }
        if tamper:
            tamper(native)
        envelope["native"] = native
        envelope["applicationImages"] = self.write(f"{label}-b20-app-images.json", {
            "sourceCommit": envelope["identity"]["commit"], "sdkLockSha256": self.identity["lockSha256"],
            "files": {"frost.exe": "a" * 64, "frost_app.dll": "a" * 64},
        })
        envelope["probeImages"] = self.write(f"{label}-b20-probe-images.json", {
            "sourceCommit": self.identity["commit"], "sdkLockSha256": self.identity["lockSha256"],
            "files": {"s1_exit.exe": "a" * 64, "s1_probe_validate.exe": "a" * 64},
        })
        return self.write(f"{label}-b20-envelope.json", envelope)

    def test_b20_uses_real_latency_sample_fields_and_independent_baseline(self):
        self.plan["budgets"]["B20"] = {"current": self.b20_native(), "baseline": self.b20_native(old=True)}
        outcome = self.cli()["budgets"]["B20"]
        self.assertEqual(outcome["state"], "over")
        self.assertEqual(outcome["currentP99Ms"], 110)
        self.assertTrue(outcome["comparativeExceeded"])
        self.assertFalse(outcome["absoluteExceeded"])

    def test_b20_rejects_unproved_workspace_and_unnatural_exit(self):
        predecessor = self.b20_native(old=True)
        for mutation, reason in (
            (lambda proof: proof["samples"].pop(), "all 20"),
            (lambda proof: proof["samples"][0].update(workspace="E:/other/llvm"), "fresh project session"),
            (lambda proof: proof["samples"][1].update(sessionIndex=proof["samples"][0]["sessionIndex"]),
             "fresh project session"),
            (lambda proof: proof["samples"][0].update(exit=1), "natural exit"),
            (lambda proof: proof["imagesSha256"].update(exitProbe="e" * 64), "reviewed build inventory"),
        ):
            with self.subTest(reason=reason):
                self.plan["budgets"]["B20"] = {"current": self.b20_native(tamper=mutation), "baseline": predecessor}
                outcome = self.cli()["budgets"]["B20"]
                self.assertEqual(outcome["state"], "invalid")
                self.assertIn(reason, outcome["reason"])

    def b28_native(self, old=False, tamper=None):
        label = "baseline" if old else "candidate"
        envelope = self.receipt(old=old, kind="B28-native")
        probe = envelope["native"]["sessionStart"]
        release = {"kind": "B28-release", "inputUnchanged": True, "sessionStart": probe, "sessionEnd": probe,
                   "initialCpuPct": 1.0, "finalCpuPct": 1.0,
                   "startUtc": envelope["startUtc"], "endUtc": envelope["endUtc"],
                   "toolsSha256": {name: "a" * 64 for name in perf_release._B28_TOOLS},
                   "toolsPath": {name: f"E:/perf-fixture/tools/{name}.exe" for name in perf_release._B28_TOOLS},
                   "runnerSha256": self.runner_sha, "sessionProbeSha256": "b" * 64}
        repo = "E:/perf-fixture/frost-source"
        build = repo + "/build/win-clangcl"
        command = [release["toolsPath"]["powershell"], "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                   repo + "/tools/verify.ps1", "-Quick", "-Jobs", "4", "-Base", "HEAD"]
        build_command = [release["toolsPath"]["cmake"], "--build", build, "--config", "Debug"]
        conditions = {"schemaVersion": 1, "budget": "B28", "variants": ["rebuild", "verify"],
                      "runs": 20, "warmup": 1, "files": frost_perf.B28_PROBE_FILES,
                      "git": {"sha": envelope["identity"]["commit"], "status_porcelain": ""},
                      "repo": repo, "build_dir": build, "verify_base_head": True, "release": release,
                      "commands": {"settle": build_command, "rebuild_all": build_command,
                                   "rebuild_frost": build_command + ["--target", "frost"], "verify": command}}
        rows = []

        def append(index, key, target, variant, value):
            execution = {"probe": len(rows) + 1, "sccache": {"misses": 1, "errors": 0, "fails": 0},
                         "settle": {"exit": 0}, "target": target, "command": command,
                         "ninja_log_edges": [{"output": perf_release._B28_OBJECTS[key]},
                                             {"output": "frost.exe"}]}
            rows.append({"schemaVersion": 1, "budget": "B28", "variant": variant, "file": key, "unit": "s",
                         "run": index if variant == "verify" else None,
                         "round": index if variant == "rebuild" else None,
                         "valid": True, "exit": 0, "counted": bool(index), "value": value, "wall_s": value,
                         "summary_line": "Verified: win & WSL", "b28": execution})

        for index in range(21):
            for key, target in frost_perf._REBUILD_ROUND:
                append(index, key, target, "rebuild", 10 if old or key != "sched" else 11)
        for index in range(21):
            append(index, ("core", "main")[index % 2], "verify", "verify", 100)

        summary = {"schemaVersion": 1, "budget": "B28", "valid": True,
                   "invalid_reason": None, "variants": {"rebuild": {}, "verify": {}}}
        if tamper:
            tamper(conditions, rows, summary)
        runs = self.tmp / f"{label}-b28-runs.jsonl"
        runs.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")
        envelope["native"] = {"conditions": self.write(f"{label}-b28-conditions.json", conditions),
                              "summary": self.write(f"{label}-b28-summary.json", summary),
                              "runs": {"file": runs.name, "sha256": digest(runs.read_bytes())}}
        return self.write(f"{label}-b28-envelope.json", envelope)

    def test_b28_compares_each_native_probe_series_without_pooling(self):
        self.plan["budgets"]["B28"] = {"current": self.b28_native(), "baseline": self.b28_native(old=True)}
        result = self.cli()["budgets"]["B28"]
        self.assertEqual(result["state"], "over")
        self.assertEqual(set(result["series"]), {"core/all", "main/all", "sched/all", "verify"})
        self.assertTrue(result["series"]["sched/all"]["comparativeExceeded"])
        self.assertFalse(result["series"]["sched/all"]["absoluteExceeded"])
        self.assertEqual(result["series"]["verify"]["pValue"], 1.0)
        (self.tmp / "candidate-b28-runs.jsonl").write_text("{}\n", encoding="utf-8")
        self.assertIn("SHA-256", self.cli()["budgets"]["B28"]["reason"])

    def test_b28_rejects_short_invalid_or_unwitnessed_runs(self):
        mutations = (
            (lambda conditions, rows, summary: rows.pop(), "every warmup"),
            (lambda conditions, rows, summary: rows[1]["b28"]["sccache"].update(misses=0), "sccache miss"),
            (lambda conditions, rows, summary: rows[1]["b28"]["ninja_log_edges"].pop(), "compilation and linking"),
            (lambda conditions, rows, summary: conditions["release"].update(initialCpuPct=12), "idle checkpoint"),
            (lambda conditions, rows, summary: conditions["release"].update(inputUnchanged=False), "input"),
            (lambda conditions, rows, summary: conditions["commands"]["verify"].append("-WindowsOnly"),
             "Quick workload"),
            (lambda conditions, rows, summary: conditions["commands"]["verify"].__setitem__(0, "C:/other/powershell.exe"),
             "Quick workload"),
        )
        predecessor = self.b28_native(old=True)
        for mutation, reason in mutations:
            with self.subTest(reason=reason):
                self.plan["budgets"]["B28"] = {"current": self.b28_native(tamper=mutation), "baseline": predecessor}
                result = self.cli()["budgets"]["B28"]
                self.assertEqual(result["state"], "invalid")
                self.assertIn(reason, result["reason"])


class MannWhitneyTest(unittest.TestCase):
    def test_exact_midrank_distribution_matches_brute_force(self):
        pooled = [1, 2, 2, 3, 5, 5]
        group = {0, 1, 2}
        pairs = list(combinations(range(len(pooled)), len(group)))

        def u(selected):
            return sum(1 if pooled[i] > pooled[j] else 0.5 if pooled[i] == pooled[j] else 0
                       for i in selected for j in range(len(pooled)) if j not in selected)

        expected = u(group)
        distance = abs(expected - 4.5)
        p = sum(abs(u(set(pair)) - 4.5) >= distance for pair in pairs) / len(pairs)
        self.assertEqual(perf_release.mann_whitney(pooled[:3], pooled[3:]), (expected, p))
        self.assertEqual(perf_release.mann_whitney([2] * 20, [2] * 20), (200.0, 1.0))


class B28InputTest(unittest.TestCase):
    def test_rewritten_ninja_log_is_invalid_even_if_new_log_is_larger(self):
        with tempfile.TemporaryDirectory() as temp:
            log = Path(temp) / ".ninja_log"
            log.write_text("# ninja log v7\n1\t2\t0\ta.obj\tde\n", encoding="utf-8")
            offset = frost_perf.ninja_log_size(log)
            tail = frost_perf.ninja_log_tail(log, offset)
            log.write_text("# ninja log v7\n3\t4\t0\ta.obj\tde\n4\t5\t0\ta.exe\tab\n", encoding="utf-8")
            with self.assertRaisesRegex(frost_perf.RunInvalid, "compacted or replaced"):
                frost_perf.timed_ninja_edges(log, offset, tail)

    def test_input_during_owned_command_prevents_timing_and_further_launches(self):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp)
            def owned(log, **kwargs):
                log.write_text("test output\n", encoding="utf-8")
                return frost_perf.Streamed(0, [], 1.0)

            launch = Mock(side_effect=lambda command, **kwargs: owned(**kwargs))
            ctx = frost_perf.RunContext(
                repo=out, build_dir=out, run_dir=out,
                tools=frost_perf.Tools(["cmake"], None, None, None, None, None),
                hooks=frost_perf.Hooks(), journal=None, files=None, runs=20, warmup=1,
                budget=frost_perf.load_budget(REPO / frost_perf.BUDGETS_FILE, "B28"), release_input_tick=123,
            )
            runner = frost_perf.B28Harness()
            with (patch.object(frost_perf, "_b14_input_tick", side_effect=[123, 124, 124]),
                  patch.object(frost_perf, "run_streamed", launch)):
                with self.assertRaisesRegex(frost_perf.RunInvalid, "input interrupted the B28 run"):
                    runner._build(ctx, None, "owned")
                self.assertTrue(ctx.log("owned").is_file())
                with self.assertRaisesRegex(frost_perf.RunInvalid, "input interrupted the B28 run"):
                    runner._build(ctx, None, "never-launched")
            launch.assert_called_once()

    def test_timeout_waits_for_native_child_and_grandchild_to_finish(self):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp)
            marker = out / "grandchild-finished"
            grandchild = "import pathlib,sys,time; time.sleep(0.2); pathlib.Path(sys.argv[1]).touch()"
            child = ("import subprocess,sys; subprocess.run([sys.executable, '-c', sys.argv[1], sys.argv[2]], "
                     "check=True); print('child-finished', flush=True)")
            result = frost_perf.run_streamed([sys.executable, "-c", child, grandchild, str(marker)],
                                            log=out / "owned.log", clock=time.perf_counter, timeout=0.05,
                                            drain_on_abort=True)
            self.assertTrue(result.timed_out)
            self.assertIsNone(result.exit)
            self.assertTrue(marker.is_file())
            self.assertIn("child-finished", (out / "owned.log").read_text(encoding="utf-8"))

    def test_interrupt_drains_grandchild_before_returning(self):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp)
            marker = out / "grandchild-finished"
            grandchild = "import pathlib,sys,time; time.sleep(0.1); pathlib.Path(sys.argv[1]).touch()"
            child = ("import subprocess,sys; print('started', flush=True); "
                     "subprocess.run([sys.executable, '-c', sys.argv[1], sys.argv[2]], check=True)")
            reads = 0

            def clock():
                nonlocal reads
                reads += 1
                if reads == 2:
                    signal.raise_signal(signal.SIGINT)  # Owned child just emitted its first line.
                return time.perf_counter()

            with self.assertRaisesRegex(frost_perf.RunInvalid, "interrupted and drained"):
                frost_perf.run_streamed([sys.executable, "-c", child, grandchild, str(marker)],
                                        log=out / "owned.log", clock=clock, timeout=10, drain_on_abort=True)
            self.assertTrue(marker.is_file())


class B20InputTest(unittest.TestCase):
    def test_desktop_input_kills_only_the_owned_native_probe(self):
        native = Mock()
        native.poll.return_value = None
        native.communicate.side_effect = frost_perf.subprocess.TimeoutExpired("probe", 0.05)
        process = unittest.mock.MagicMock()
        process.__enter__.return_value = native
        with (patch.object(frost_perf.subprocess, "Popen", return_value=process),
              patch.object(frost_perf, "_b14_input_tick", side_effect=[123, 123, 124]),
              self.assertRaisesRegex(frost_perf.RunInvalid, "input interrupted the B20 run")):
            frost_perf._b20_sample(["s1_exit"], Path("."), {}, 123)
        native.kill.assert_called_once()


if __name__ == "__main__":
    unittest.main()
