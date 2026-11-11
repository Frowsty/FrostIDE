# SPDX-License-Identifier: Apache-2.0
# Managed fixture selection, verifier wiring and zero-launch refusal; no real SDK/provider execution.
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest import mock

import managed_fixture
from tools_test_support import TempDirTestCase


FEED = "ManagedE2E.InheritedProfilePreservesCustomNugetFeed"
RENAME = "ManagedE2E.UnsavedCrossProjectUtf16NavigationAndRename"
DEBUG = "WorkbenchManaged.RealConsoleProjectBuildsAndDebugsWithVerifiedSource"


class ManagedFixtureTest(TempDirTestCase):
    def fixture(self) -> dict:
        """Synthetic file inputs exercise the launcher; mocked verifiers never claim real provenance."""
        root = self.tmp / "repository"
        (root / "sdk").mkdir(parents=True)
        (root / "sdk/sdk.lock.json").write_text(json.dumps({
            "windows": {"dotnet": {"version": "10.0.403"}},
            "linux": {"dotnet": {"version": "10.0.403"}}}), encoding="utf-8")
        sdk = self.tmp / "sdk with spaces"
        (sdk / "sdk/10.0.403").mkdir(parents=True)
        (sdk / "sdk/10.0.403/dotnet.dll").write_bytes(b"SDK fixture")
        dotnet = sdk / ("dotnet.exe" if os.name == "nt" else "dotnet")
        dotnet.write_bytes(b"not executed")
        server = self.tmp / "server/CSharpLanguageServer.dll"
        server.parent.mkdir()
        server.write_bytes(b"not executed")
        archive = self.tmp / "server.nupkg"
        archive.write_bytes(b"verifier argument fixture")
        payload = self.tmp / "payload"
        (payload / "adapter").mkdir(parents=True)
        (payload / "PdbGuard").mkdir()
        adapter = payload / "adapter" / ("netcoredbg.exe" if os.name == "nt" else "netcoredbg")
        adapter.write_bytes(b"not executed")
        guard = payload / "PdbGuard/PdbGuard.dll"
        guard.write_bytes(b"not executed")
        return {"schemaVersion": 1, "sourceRoot": str(root), "tools": {
            "FROST_TEST_CSHARP_DOTNET": str(dotnet),
            "FROST_TEST_CSHARP_SERVER": str(server),
            "FROST_TEST_CSHARP_ARCHIVE": str(archive),
            "FROST_TEST_MANAGED_DEBUG_ADAPTER": str(adapter),
            "FROST_TEST_MANAGED_DEBUG_GUARD": str(guard),
            "FROST_MANAGED_DEBUG_MANIFEST_SHA256": "a" * 64},
            "cases": {FEED: ["sdk"], RENAME: ["sdk", "server"], DEBUG: ["sdk", "debugger"]}}

    def test_discovery_and_unrelated_cases_need_no_tools_or_source_access(self) -> None:
        cases = {FEED: ["sdk"]}
        self.assertEqual(managed_fixture.requirements(["--gtest_list_tests"], {}, cases), set())
        self.assertEqual(managed_fixture.requirements(["--gtest_filter=Unrelated.*"], {}, cases), set())
        self.assertEqual(managed_fixture.fixture_environment({}, {"EXISTING": "value"}, set(), Path("unused")),
                         {"EXISTING": "value"})

    def test_only_selected_case_requirements_are_combined(self) -> None:
        cases = {FEED: ["sdk"], RENAME: ["sdk", "server"], DEBUG: ["sdk", "debugger"]}
        self.assertEqual(managed_fixture.requirements([f"--gtest_filter={FEED}"], {}, cases), {"sdk"})
        self.assertEqual(managed_fixture.requirements([f"--gtest_filter=ManagedE2E.*-{FEED}"], {}, cases),
                         {"sdk", "server"})
        self.assertEqual(managed_fixture.requirements([], {"GTEST_FILTER": DEBUG}, cases), {"sdk", "debugger"})
        self.assertEqual(managed_fixture.requirements(["--gtest_filter=-WorkbenchManaged.*"], {}, cases),
                         {"sdk", "server"})
        self.assertEqual(managed_fixture.requirements(["--gtest_filter=ManagedE2E.[IU]*"], {}, cases), set())
        self.assertEqual(managed_fixture.requirements(["--gtest_filter=*-[MW]*"], {}, cases),
                         {"sdk", "server", "debugger"})
        self.assertEqual(managed_fixture.requirements([f"--gtest_filter={FEED}"], {"GTEST_FILTER": DEBUG}, cases),
                         {"sdk"})

    def test_discovery_uses_last_boolean_flag_and_pinned_false_prefixes(self) -> None:
        cases = {FEED: ["sdk"]}
        for value in ("0", "0anything", "f", "false", "F", "FALSE"):
            with self.subTest(value=value):
                self.assertEqual(managed_fixture.requirements(
                    ["--gtest_list_tests", f"--gtest_list_tests={value}"], {}, cases), {"sdk"})
        for value in ("", "1", "true", "no", "off"):
            with self.subTest(value=value):
                self.assertEqual(managed_fixture.requirements(
                    ["--gtest_list_tests=0", f"--gtest_list_tests={value}"], {}, cases), set())
        self.assertEqual(managed_fixture.requirements(
            ["--gtest_list_tests=0", "--gtest_list_tests"], {}, cases), set())
        # list_tests has a literal false default in pinned GTest, not an environment-derived one.
        self.assertEqual(managed_fixture.requirements([], {"GTEST_LIST_TESTS": "1"}, cases), {"sdk"})

    def test_only_equals_filter_updates_selection_and_last_occurrence_wins(self) -> None:
        cases = {FEED: ["sdk"], DEBUG: ["sdk", "debugger"]}
        self.assertEqual(managed_fixture.requirements(
            [f"--gtest_filter={FEED}", f"--gtest_filter={DEBUG}"], {"GTEST_FILTER": "Unrelated.*"}, cases),
                         {"sdk", "debugger"})
        self.assertEqual(managed_fixture.requirements(
            [f"--gtest_filter={DEBUG}", "--gtest_filter=Unrelated.*"], {}, cases), set())
        # GTest requires '=' for string flags. A separate word cannot narrow our preflight.
        self.assertEqual(managed_fixture.requirements(["--gtest_filter", "Unrelated.*"], {}, cases),
                         {"sdk", "debugger"})
        self.assertEqual(managed_fixture.requirements(["--gtest_filter", DEBUG], {"GTEST_FILTER": FEED}, cases),
                         {"sdk"})

    def test_indirect_flagfile_is_rejected_even_for_discovery_or_unrelated_selection(self) -> None:
        cases = {FEED: ["sdk"]}
        for arguments in (["--gtest_list_tests", "--gtest_flagfile=flags"],
                          ["--gtest_flagfile=flags", "--gtest_list_tests"],
                          ["--gtest_filter=Unrelated.*", "--gtest_flagfile=flags"],
                          ["--gtest_flagfile", "flags"]):
            with self.subTest(arguments=arguments), self.assertRaisesRegex(ValueError, "--gtest_flagfile"):
                managed_fixture.requirements(arguments, {}, cases)

    def test_explicit_empty_tool_does_not_fall_back(self) -> None:
        configuration = self.fixture()
        with self.assertRaisesRegex(ValueError, "FROST_TEST_CSHARP_DOTNET"):
            managed_fixture.fixture_environment(configuration, {"FROST_TEST_CSHARP_DOTNET": ""},
                                                {"sdk"}, self.tmp / "test")

    def test_sdk_requires_the_locked_version_directory(self) -> None:
        configuration = self.fixture()
        dotnet = Path(configuration["tools"]["FROST_TEST_CSHARP_DOTNET"])
        (dotnet.parent / "sdk/10.0.403/dotnet.dll").unlink()
        with self.assertRaisesRegex(ValueError, "pinned sdk/10.0.403"):
            managed_fixture.fixture_environment(configuration, {}, {"sdk"}, self.tmp / "test")

    def test_server_uses_original_installed_archive_verifier(self) -> None:
        configuration = self.fixture()
        with mock.patch.object(managed_fixture.managed_server, "verify_installed") as verify:
            environment = managed_fixture.fixture_environment(configuration, {"EXISTING": "value"},
                                                                {"sdk", "server"}, self.tmp / "test")
        tools = configuration["tools"]
        verify.assert_called_once_with(Path(tools["FROST_TEST_CSHARP_ARCHIVE"]),
                                       Path(tools["FROST_TEST_CSHARP_SERVER"]).parent,
                                       Path(configuration["sourceRoot"]))
        self.assertEqual(environment["FROST_TEST_CSHARP_SERVER"], tools["FROST_TEST_CSHARP_SERVER"])
        self.assertEqual(environment["EXISTING"], "value")
        self.assertNotIn("FROST_TEST_MANAGED_DEBUG_ADAPTER", environment)

    def test_missing_server_archive_and_failed_verification_refuse(self) -> None:
        configuration = self.fixture()
        with mock.patch.object(managed_fixture.managed_server, "verify_installed", side_effect=ValueError("tampered")):
            with self.assertRaisesRegex(ValueError, "tampered"):
                managed_fixture.fixture_environment(configuration, {}, {"sdk", "server"}, self.tmp / "test")
        Path(configuration["tools"]["FROST_TEST_CSHARP_ARCHIVE"]).unlink()
        with self.assertRaisesRegex(ValueError, "FROST_TEST_CSHARP_ARCHIVE"):
            managed_fixture.fixture_environment(configuration, {}, {"sdk", "server"}, self.tmp / "test")

    def test_payload_requires_independent_digest_and_matching_guard(self) -> None:
        configuration = self.fixture()
        with mock.patch.object(managed_fixture, "verify_debug_payload") as verify:
            with self.assertRaisesRegex(ValueError, "independently reviewed build digest"):
                managed_fixture.fixture_environment(configuration, {"FROST_MANAGED_DEBUG_MANIFEST_SHA256": ""},
                                                    {"sdk", "debugger"}, self.tmp / "test")
            verify.assert_not_called()
            managed_fixture.fixture_environment(configuration, {}, {"sdk", "debugger"}, self.tmp / "test")
            verify.assert_called_once_with(self.tmp / "payload", "a" * 64)
        other = self.tmp / "other/PdbGuard.dll"
        other.parent.mkdir()
        other.write_bytes(b"different payload")
        with self.assertRaisesRegex(ValueError, "one reviewed payload"):
            managed_fixture.fixture_environment(configuration, {"FROST_TEST_MANAGED_DEBUG_GUARD": str(other)},
                                                {"sdk", "debugger"}, self.tmp / "test")

    def test_bundle_mode_validates_the_actual_executable_sibling(self) -> None:
        configuration = self.fixture()
        executable = self.tmp / "app/test"
        with self.assertRaisesRegex(ValueError, "bundled managed adapter"):
            managed_fixture.fixture_environment(configuration, {"FROST_TEST_MANAGED_USE_BUNDLE": "1"},
                                                {"sdk", "debugger"}, executable)
        bundle = executable.parent / "FrostManagedDebug"
        bundle.parent.mkdir()
        (self.tmp / "payload").rename(bundle)
        with mock.patch.object(managed_fixture, "verify_debug_payload") as verify:
            managed_fixture.fixture_environment(configuration, {"FROST_TEST_MANAGED_USE_BUNDLE": "1"},
                                                {"sdk", "debugger"}, executable)
            verify.assert_called_once_with(bundle, "a" * 64)

    def test_validation_failure_starts_no_test_process(self) -> None:
        configuration = self.fixture()
        configuration["tools"]["FROST_TEST_CSHARP_DOTNET"] = ""
        record = self.tmp / "config.json"
        record.write_text(json.dumps(configuration), encoding="utf-8")
        with mock.patch.object(sys, "argv", ["managed_fixture", "--config", str(record), "--", "test",
                                             f"--gtest_filter={FEED}"]), mock.patch.dict(os.environ, {}, clear=True), \
                mock.patch.object(managed_fixture, "run_test") as launch:
            self.assertEqual(managed_fixture.main(), 1)
            launch.assert_not_called()

    def test_cancelled_discovery_requires_provenance_before_any_test_process(self) -> None:
        configuration = self.fixture()
        configuration["tools"]["FROST_TEST_CSHARP_DOTNET"] = ""
        record = self.tmp / "config.json"
        record.write_text(json.dumps(configuration), encoding="utf-8")
        command = ["test", "--gtest_list_tests", "--gtest_list_tests=0", f"--gtest_filter={FEED}"]
        with mock.patch.object(sys, "argv", ["managed_fixture", "--config", str(record), "--", *command]), \
                mock.patch.dict(os.environ, {}, clear=True), \
                mock.patch.object(managed_fixture, "run_test") as launch:
            self.assertEqual(managed_fixture.main(), 1)
            launch.assert_not_called()

    def test_flagfile_override_cannot_start_a_test_or_inspect_tools(self) -> None:
        record = self.tmp / "config.json"
        record.write_text(json.dumps({"schemaVersion": 1, "cases": {FEED: ["sdk"]}}), encoding="utf-8")
        flags = self.tmp / "selection.flags"
        flags.write_text(f"--gtest_list_tests=0\n--gtest_filter={FEED}\n", encoding="utf-8")
        command = ["test", "--gtest_list_tests", f"--gtest_flagfile={flags}"]
        with mock.patch.object(sys, "argv", ["managed_fixture", "--config", str(record), "--", *command]), \
                mock.patch.object(managed_fixture, "fixture_environment") as inspect, \
                mock.patch.object(managed_fixture, "run_test") as launch:
            self.assertEqual(managed_fixture.main(), 1)
            inspect.assert_not_called()
            launch.assert_not_called()

    def test_discovery_preserves_command_and_exit_code(self) -> None:
        record = self.tmp / "config.json"
        record.write_text(json.dumps({"schemaVersion": 1, "cases": {FEED: ["sdk"]}}), encoding="utf-8")
        command = ["test", "--gtest_list_tests"]
        with mock.patch.object(sys, "argv", ["managed_fixture", "--config", str(record), "--", *command]), \
                mock.patch.object(managed_fixture, "run_test", return_value=17) as launch:
            self.assertEqual(managed_fixture.main(), 17)
            self.assertEqual(launch.call_args.args[0], command)

    def test_final_true_discovery_preserves_command_without_fixture_inputs(self) -> None:
        record = self.tmp / "config.json"
        record.write_text(json.dumps({"schemaVersion": 1, "cases": {FEED: ["sdk"]}}), encoding="utf-8")
        command = ["test", "--gtest_list_tests=0", "--gtest_list_tests", f"--gtest_filter={FEED}"]
        with mock.patch.object(sys, "argv", ["managed_fixture", "--config", str(record), "--", *command]), \
                mock.patch.object(managed_fixture, "run_test", return_value=17) as launch:
            self.assertEqual(managed_fixture.main(), 17)
            self.assertEqual(launch.call_args.args[0], command)

    def test_unrelated_selection_preserves_command_without_fixture_inputs(self) -> None:
        record = self.tmp / "config.json"
        record.write_text(json.dumps({"schemaVersion": 1, "cases": {FEED: ["sdk"]}}), encoding="utf-8")
        command = ["test", "--gtest_filter=Unrelated.*", "--gtest_list_tests=0"]
        with mock.patch.object(sys, "argv", ["managed_fixture", "--config", str(record), "--", *command]), \
                mock.patch.dict(os.environ, {"GTEST_FILTER": FEED}, clear=True), \
                mock.patch.object(managed_fixture, "run_test", return_value=17) as launch:
            self.assertEqual(managed_fixture.main(), 17)
            self.assertEqual(launch.call_args.args[0], command)

    def test_interruption_waits_for_original_test_cleanup(self) -> None:
        child = mock.Mock()
        child.wait.side_effect = [KeyboardInterrupt, subprocess.TimeoutExpired("test", 5), 0]
        child.poll.return_value = None
        with mock.patch.object(managed_fixture.subprocess, "Popen", return_value=child):
            with self.assertRaises(KeyboardInterrupt):
                managed_fixture.run_test(["test"], {})
        child.terminate.assert_called_once_with()
        child.kill.assert_called_once_with()
        self.assertEqual(child.wait.call_args_list, [mock.call(), mock.call(timeout=5), mock.call()])


if __name__ == "__main__":
    unittest.main()
