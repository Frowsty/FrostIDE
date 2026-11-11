# SPDX-License-Identifier: Apache-2.0
# Portable unit tests for fixture preservation/runner policy. Doubles provide no Unity acceptance.
from __future__ import annotations

import base64
import json
import os
from pathlib import Path
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import run_legacy_unity_fixtures as runner
import windows_fixture_guard as guard


class UnitCipher:
    """A reversible unit double, used exclusively by policy tests instead of Windows DPAPI."""

    def protect(self, value: bytes) -> bytes:
        return b"unit-cipher:" + base64.b64encode(value)

    def unprotect(self, value: bytes) -> bytes:
        if not value.startswith(b"unit-cipher:"):
            raise ValueError("Invalid unit cipher input")
        return base64.b64decode(value.removeprefix(b"unit-cipher:"), validate=True)


class UnitRegistry:
    def __init__(self, snapshot: guard.Snapshot) -> None:
        self.current = snapshot
        self.restores = 0
        self.leave_difference = False

    def capture(self) -> guard.Snapshot:
        return guard.Snapshot(self.current.keys, dict(self.current.values))

    def restore(self, expected: guard.Snapshot, baseline: guard.Snapshot) -> None:
        if self.current != expected:
            raise guard.GuardError("Unit registry changed before restore")
        self.restores += 1
        if not self.leave_difference:
            self.current = baseline


class UnitJob:
    def __init__(self, members: set[guard.ProcessIdentity] | None = None) -> None:
        self.members = members or set()

    def owns(self, process: guard.ProcessIdentity) -> bool:
        return process in self.members

    def empty(self) -> bool:
        return not self.members


class PreferenceGuardPolicy(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.baseline = guard.Snapshot(frozenset({"", "empty"}), {
            ("", "binary"): guard.TypedValue(3, b"\x00\xffprivate-unit-bytes"),
            ("", "string"): guard.TypedValue(1, "café\0".encode("utf-16-le")),
            ("", "multi"): guard.TypedValue(7, "one\0two\0\0".encode("utf-16-le")),
        })
        self.registry = UnitRegistry(self.baseline)
        self.writers = set()
        self.policy = guard.PreferenceGuard(self.directory, self.registry, UnitCipher(), lambda: self.writers.copy())
        self.addCleanup(self.stop_monitor)

    def stop_monitor(self) -> None:
        self.policy.finished.set()
        if self.policy.thread is not None:
            self.policy.thread.join()

    def test_snapshot_roundtrips_exact_types_bytes_empty_keys_and_absence(self) -> None:
        self.assertEqual(guard.Snapshot.decode(self.baseline.encode()), self.baseline)
        absent = guard.Snapshot(frozenset(), {})
        self.assertEqual(guard.Snapshot.decode(absent.encode()), absent)

    def test_baseline_is_durable_and_decodable_before_job_admission(self) -> None:
        self.policy.begin()
        saved = (self.directory / "preferences-before.dpapi").read_bytes()
        self.assertNotIn(b"private-unit-bytes", saved)
        self.assertEqual(guard.Snapshot.decode(UnitCipher().unprotect(saved)), self.baseline)
        self.policy.add_job(UnitJob())
        self.assertEqual(self.policy.restore()["finalDifferences"], 0)

    def test_fsync_precedes_job_admission(self) -> None:
        with patch.object(guard.os, "fsync", wraps=guard.os.fsync) as sync:
            self.policy.begin()
            self.assertGreaterEqual(sync.call_count, 1)
        self.policy.add_job(UnitJob())
        self.policy.restore()

    def test_uncaptured_or_unflushed_baseline_cannot_admit_job(self) -> None:
        with self.assertRaises(guard.GuardError):
            self.policy.add_job(UnitJob())
        self.policy.baseline = self.baseline
        with self.assertRaises(guard.GuardError):
            self.policy.add_job(UnitJob())

    def test_failed_snapshot_write_cannot_admit_job(self) -> None:
        with patch.object(guard, "durable_write", side_effect=OSError):
            with self.assertRaises(OSError):
                self.policy.begin()
        with self.assertRaises(guard.GuardError):
            self.policy.add_job(UnitJob())
        self.assertEqual(self.registry.restores, 0)

    def test_existing_known_writer_refuses_before_snapshot_or_launch(self) -> None:
        self.writers.add(guard.ProcessIdentity(10, 123, "unity.exe"))
        with self.assertRaises(guard.GuardError):
            self.policy.begin()
        self.assertFalse((self.directory / "preferences-before.dpapi").exists())
        self.assertEqual(self.registry.restores, 0)

    def test_live_owned_job_blocks_all_restoration(self) -> None:
        self.policy.begin()
        identity = guard.ProcessIdentity(10, 123, "unity.exe")
        self.policy.add_job(UnitJob({identity}))
        with self.assertRaises(guard.GuardError):
            self.policy.restore()
        self.assertEqual(self.registry.restores, 0)

    def test_pid_reuse_does_not_authorize_unowned_writer(self) -> None:
        self.policy.begin()
        self.policy.add_job(UnitJob({guard.ProcessIdentity(10, 123, "unity.exe")}))
        self.writers.add(guard.ProcessIdentity(10, 124, "unity.exe"))
        with self.assertRaises(guard.GuardError):
            self.policy.check_writers()

    def test_observed_foreign_writer_stays_unsafe_after_it_exits(self) -> None:
        self.policy.begin()
        self.writers.add(guard.ProcessIdentity(20, 222, "unityhub.exe"))
        with self.assertRaises(guard.GuardError):
            self.policy.check_writers()
        self.writers.clear()
        with self.assertRaises(guard.GuardError):
            self.policy.restore()
        self.assertEqual(self.registry.restores, 0)

    def test_complete_current_typed_baseline_restored_after_empty_jobs(self) -> None:
        self.policy.begin()
        self.policy.add_job(UnitJob())
        self.registry.current = guard.Snapshot(frozenset({"", "new"}), {
            ("", "string"): guard.TypedValue(4, b"\x01\x00\x00\x00"),
            ("new", "created"): guard.TypedValue(3, b"new-unit-value"),
        })
        result = self.policy.restore()
        self.assertEqual(self.registry.current, self.baseline)
        self.assertEqual(result["changedValues"], 4)
        self.assertEqual(result["finalDifferences"], 0)
        self.assertNotIn("private-unit-bytes", json.dumps(result))

    def test_changed_durable_baseline_is_refused(self) -> None:
        self.policy.begin()
        different = guard.Snapshot(frozenset({""}), {})
        (self.directory / "preferences-before.dpapi").write_bytes(UnitCipher().protect(different.encode()))
        with self.assertRaises(guard.GuardError):
            self.policy.restore()
        self.assertEqual(self.registry.restores, 0)

    def test_final_difference_is_a_failure(self) -> None:
        self.policy.begin()
        self.registry.current = guard.Snapshot(frozenset({""}), {})
        self.registry.leave_difference = True
        with self.assertRaises(guard.GuardError):
            self.policy.restore()


class LegacyRunnerPolicy(unittest.TestCase):
    def test_scratch_unicode_source_preserves_existing_exact_position(self) -> None:
        runner.validate_fixture_position(runner.fixture_source())
        with self.assertRaises(guard.GuardError):
            runner.validate_fixture_position(runner.fixture_source().replace("\U0001f600", "x"))

    def test_environment_preserves_entitlement_home_and_removes_stale_fixture_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            environment = {"HOME": "inherited-home", "USERPROFILE": "inherited-user",
                           "FROST_TEST_UNITY_PROJECT": "old-project", "FROST_UNITY_EXCHANGE": "old-exchange"}
            with patch.dict(runner.os.environ, environment, clear=True):
                actual = runner.base_environment(Path(temporary))
            self.assertEqual(actual["HOME"], "inherited-home")
            self.assertEqual(actual["USERPROFILE"], "inherited-user")
            self.assertNotIn("FROST_TEST_UNITY_PROJECT", actual)
            self.assertNotIn("FROST_UNITY_EXCHANGE", actual)
            self.assertNotEqual(actual["APPDATA"], "inherited-user")

    def test_exact_case_success_required_and_skip_cannot_pass(self) -> None:
        case = runner.CASES["mono"]
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "log"
            path.write_text("[       OK ] " + case)
            runner.check_test_log(path, case)
            path.write_text("[  SKIPPED ] " + case)
            with self.assertRaises(guard.GuardError):
                runner.check_test_log(path, case)

    def test_typed_control_observations_and_all_seven_workbench_records_required(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            exchange = Path(temporary)
            for index, checkpoint in enumerate(runner.CHECKPOINTS):
                value = {"checkpoint": checkpoint, "playing": checkpoint in (
                    "playing", "paused", "stepped", "resumed"), "paused": checkpoint in ("paused", "stepped"),
                         "generation": 1 if index == 0 else 2, "frame": index}
                (exchange / checkpoint).write_text(json.dumps(value))
            records = exchange / "workbench-assertions.jsonl"
            records.write_text("".join(json.dumps({"checkpoint": name}) + "\n" for name in runner.CHECKPOINTS))
            self.assertEqual(len(runner.validate_observations(exchange)), 7)
            value = json.loads((exchange / "stepped").read_text())
            value["frame"] = 2
            (exchange / "stepped").write_text(json.dumps(value))
            with self.assertRaises(guard.GuardError):
                runner.validate_observations(exchange)

    def test_known_editor_input_hashes_are_full_sha256_values(self) -> None:
        for value in runner.INPUT_HASHES.values():
            self.assertRegex(value, r"^[0-9a-f]{64}$")

    def test_interrupted_begin_or_workflow_records_failure_and_cleans_up(self) -> None:
        for phase in ("begin", "workflow"):
            with self.subTest(phase=phase), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary).resolve()
                executable = root / "unit-executable"
                executable.write_bytes(b"unit input")
                scratch = root / "fresh"
                args = SimpleNamespace(unity_editor=executable, test_executable=executable,
                                       frost_executable=executable, scratch=scratch,
                                       workflow="mono", import_timeout=600)
                registry = UnitRegistry(guard.Snapshot(frozenset({""}), {}))
                policy = guard.PreferenceGuard(scratch, registry, UnitCipher(), lambda: set())
                with (patch.object(runner, "windows_only"), patch.object(runner, "validate_editor", return_value={}),
                      patch.object(runner, "restrict_current_user"), patch.object(runner, "WindowsPreferences"),
                      patch.object(runner, "Dpapi"), patch.object(runner, "PreferenceGuard", return_value=policy),
                      patch.object(runner, "run_mono", side_effect=KeyboardInterrupt)):
                    if phase == "begin":
                        with patch.object(policy, "begin", side_effect=KeyboardInterrupt):
                            with self.assertRaises(KeyboardInterrupt):
                                runner.execute(args)
                    else:
                        with self.assertRaises(KeyboardInterrupt):
                            runner.execute(args)
                evidence = json.loads((scratch / "result.json").read_text())
                self.assertEqual(evidence["status"], "failed")
                self.assertTrue(evidence["interrupted"])
                self.assertEqual(evidence["cases"], [])
                self.assertTrue(evidence["ownedJobsEmpty"])
                self.assertEqual(registry.restores, 1 if phase == "workflow" else 0)


@unittest.skipUnless(os.name == "nt", "Windows DPAPI and job objects are platform-specific")
class WindowsGuardPrimitives(unittest.TestCase):
    """Actual OS primitive oracles over private data/Python children; no Editor or user prefs."""

    def test_current_user_dpapi_roundtrip_and_ciphertext(self) -> None:
        cipher = guard.Dpapi()
        plain = b"private unit fixture bytes\x00\xff"
        encrypted = cipher.protect(plain)
        self.assertNotIn(plain, encrypted)
        self.assertEqual(cipher.unprotect(encrypted), plain)

    def test_assigned_job_contains_real_child_and_descendant_before_restore(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            guard.restrict_current_user(directory)
            registry = UnitRegistry(guard.Snapshot(frozenset({""}), {}))
            policy = guard.PreferenceGuard(directory, registry, guard.Dpapi(), lambda: set())
            policy.begin()
            job = guard.OwnedJob(policy)
            try:
                script = ("import subprocess,sys,time; "
                          "child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)']); "
                          "print(child.pid,flush=True); time.sleep(30)")
                log = directory / "child.log"
                job.start([sys.executable, "-c", script], dict(os.environ), directory, log)
                deadline = time.monotonic() + 10
                child = None
                while time.monotonic() < deadline:
                    text = log.read_text().strip()
                    if text:
                        child = int(text)
                        break
                    time.sleep(0.02)
                self.assertIsNotNone(child)
                self.assertEqual(job.pids(), {job.pid, child})
                self.assertFalse(job.empty())
                self.assertEqual(registry.restores, 0)
            finally:
                job.terminate()
                self.assertTrue(job.empty())
                policy.restore()
                job.close()
            self.assertEqual(registry.restores, 1)


if __name__ == "__main__":
    unittest.main()
